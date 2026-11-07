#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Calibrate aiming from the fixed lens: where does each motor position look, in its picture?

Host tool, run once per camera. It turns the pan/tilt lens through a grid of positions,
always arriving from the same side so gear backlash is taken up the same way, takes a
still from both lenses at each stop, finds the pan/tilt picture inside the fixed lens's by
feature matching, fits the motor steps as a quadratic of the picture position and stores
that on the camera. A first pass finds where the two pictures overlap and a finer one fills
that in. The lens goes back to where it was. It takes about 5 minutes: run it in daylight,
with some detail in view, and pause anything that steers the camera, such as an NVR's
auto-tracking.

Needs OpenCV and NumPy (pip install opencv-python-headless numpy).
The password of the stream account comes from CAMERA_PASSWORD.
"""
import argparse
import base64
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.request

import cv2
import numpy as np

SIZE = (640, 360)  # both stills are compared at this size, whatever the streams are
MIN_INLIERS = 40


class Camera:
    def __init__(self, address, user, password):
        self.base = f"https://{address}:18443"
        self.auth = {"Authorization": "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()}
        self.tls = ssl.create_default_context()
        self.tls.check_hostname = False  # the camera signs its own certificate
        self.tls.verify_mode = ssl.CERT_NONE

    def request(self, path, body=None):
        headers = dict(self.auth)
        if body is not None:
            headers["Content-Type"] = "application/json"
        data = None if body is None else json.dumps(body).encode()
        with urllib.request.urlopen(urllib.request.Request(self.base + path, data, headers), context=self.tls, timeout=20) as reply:
            return reply.read()

    def json(self, path, body=None):
        return json.loads(self.request(path, body))

    def still(self, channel):
        for _ in range(30):
            try:
                body = self.request(f"/api/v1/snapshot?channel={channel}")
                break
            except urllib.error.HTTPError as error:  # someone else's stills: not worth losing a scan over
                if error.code != 503:
                    raise
                time.sleep(1)
        else:
            raise RuntimeError("the camera stayed busy with other stills")
        data = np.frombuffer(body, np.uint8)
        return cv2.imdecode(data, cv2.IMREAD_GRAYSCALE)

    def goto(self, x, y):
        self.json("/api/v1/aim", {"steps": [x, y]})
        for _ in range(200):
            time.sleep(0.2)
            if not self.json("/api/v1/status")["platform"]["moving"]:
                return
        raise SystemExit("the motors did not stop")


def scan(camera, xs, ys):
    stops = []
    for y in ys:
        camera.goto(xs[0] - 120, y)
        for x in xs:
            camera.goto(x, y)
            time.sleep(0.7)
            moved_from = camera.json("/api/v1/aim")["steps"]
            head, fixed = camera.still(0), camera.still(2)
            if camera.json("/api/v1/aim")["steps"] == moved_from:  # an NVR may be steering too
                stops.append((moved_from, head, fixed))
            print(f"  {x},{y}", flush=True)
    return stops


def locate(head, fixed, sift, matcher):
    """The pan/tilt picture's centre in the fixed lens's picture, if they match well."""
    head = cv2.resize(head, SIZE, interpolation=cv2.INTER_AREA)
    fixed = cv2.resize(fixed, SIZE, interpolation=cv2.INTER_AREA)
    kh, dh = sift.detectAndCompute(head, None)
    kf, df = sift.detectAndCompute(fixed, None)
    if dh is None or df is None or len(kh) < 8 or len(kf) < 8:
        return None
    good = [m[0] for m in matcher.knnMatch(dh, df, k=2) if len(m) == 2 and m[0].distance < 0.75 * m[1].distance]
    if len(good) < MIN_INLIERS:
        return None
    H, mask = cv2.findHomography(np.float32([kh[m.queryIdx].pt for m in good]),
                                 np.float32([kf[m.trainIdx].pt for m in good]), cv2.RANSAC, 3.0)
    if H is None or int(mask.sum()) < MIN_INLIERS:
        return None
    w, h = SIZE
    corners = np.float32([[0, 0], [w, 0], [0, h], [w / 2, h / 2]]).reshape(-1, 1, 2)
    c = cv2.perspectiveTransform(corners, H).reshape(-1, 2)
    # A real match is about one picture across; anything else is a bad homography.
    if not (0.8 * w <= np.linalg.norm(c[1] - c[0]) <= 1.15 * w and 0.8 * h <= np.linalg.norm(c[2] - c[0]) <= 1.2 * h):
        return None
    return c[3]


def matched(stops):
    sift, matcher = cv2.SIFT_create(3000), cv2.BFMatcher(cv2.NORM_L2)
    points = []
    for steps, head, fixed in stops:
        centre = locate(head, fixed, sift, matcher)
        if centre is not None:
            points.append((steps, centre))
    return points


def fit(points):
    m = np.array([p[0] for p in points], float)
    c = np.array([p[1] for p in points], float)
    u, v = c[:, 0] / SIZE[0] * 2 - 1, c[:, 1] / SIZE[1] * 2 - 1
    X = np.stack([np.ones_like(u), u, v, u * v, u * u, v * v], 1)
    coef, *_ = np.linalg.lstsq(X, m, rcond=None)
    rms = np.sqrt(((m - X @ coef) ** 2).mean(0))
    return coef[:, 0].round().astype(int).tolist(), coef[:, 1].round().astype(int).tolist(), rms


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--camera", required=True, help="the camera's address")
    p.add_argument("--user", default="admin", help="stream account")
    p.add_argument("--x", default="1500:3300:300", metavar="FROM:TO:STEP", help="pan steps to visit (default %(default)s)")
    p.add_argument("--y", default="60:660:150", metavar="FROM:TO:STEP", help="tilt steps to visit (default %(default)s)")
    p.add_argument("--dry-run", action="store_true", help="scan and fit, but store nothing")
    a = p.parse_args()
    password = os.environ.get("CAMERA_PASSWORD")
    if not password:
        sys.exit("set CAMERA_PASSWORD")
    def axis(text):
        lo, hi, step = map(int, text.split(":"))
        return list(range(lo, hi + 1, step))
    camera = Camera(a.camera, a.user, password)
    start = camera.json("/api/v1/aim")["steps"]
    if not start:
        sys.exit("the camera does not know its position yet")
    xs, ys = axis(a.x), axis(a.y)
    try:
        points = matched(scan(camera, xs, ys))
        print(f"first pass: {len(points)} stops matched")
        if len(points) >= 3:
            # between the first pass's stops, around the ones that matched
            sx, sy = xs[1] - xs[0], ys[1] - ys[0]
            mx, my = [p[0][0] for p in points], [p[0][1] for p in points]
            points += matched(scan(camera, list(range(min(mx) - sx // 2, max(mx) + sx // 2 + 1, sx)),
                                   list(range(max(0, min(my) - sy // 2), max(my) + sy // 2 + 1, sy))))
    finally:
        camera.goto(*start)
    print(f"{len(points)} stops matched in all")
    if len(points) < 9:
        sys.exit("too few stops matched: more detail in view, more light, or another grid (--x, --y)")
    x, y, rms = fit(points)
    print("x", x, "\ny", y, f"\nfit error, in steps: pan {rms[0]:.0f}, tilt {rms[1]:.0f}")
    if a.dry_run:
        return
    camera.json("/api/v1/aim", {"x": x, "y": y})
    print("stored on the camera")


if __name__ == "__main__":
    main()
