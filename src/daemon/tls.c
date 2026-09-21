#define _GNU_SOURCE
#include "joan_daemon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef JOAN_NO_TLS
int joan_tls_ensure_identity(const JoanConfig *cfg) { (void)cfg; return -1; }
int joan_tls_enroll_identity(const JoanConfig *cfg,const char *pem,size_t len,char *why,size_t why_len)
{ (void)cfg;(void)pem;(void)len;snprintf(why,why_len,"built without TLS");return -1; }
int joan_tls_clear_identity(const JoanConfig *cfg) { (void)cfg; return -1; }
#else
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/oid.h>
#include <mbedtls/x509_crt.h>

int joan_tls_ensure_identity(const JoanConfig *cfg)
{
    char key_path[512], cert_path[512],host_path[512],mark_path[512],label[64],dns[80]; FILE *f;
    mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context key; mbedtls_x509write_cert crt; mbedtls_mpi serial;
    unsigned char key_pem[2048], cert_pem[4096], serial_bytes[16],san[96];
    unsigned char *saved=NULL;size_t saved_len=0,san_len,dns_len;
    const char *personal="joan-local-device-identity"; int rc=-1;
    snprintf(key_path,sizeof(key_path),"%s/tls-key.pem",cfg->state_dir);
    snprintf(cert_path,sizeof(cert_path),"%s/tls-cert.pem",cfg->state_dir);
    snprintf(host_path,sizeof(host_path),"%s/tls-hostname",cfg->state_dir);
    joan_mdns_get_configured_hostname(cfg,label);snprintf(dns,sizeof(dns),"%s.local",label);
    snprintf(mark_path,sizeof(mark_path),"%s/tls-enrolled",cfg->state_dir);
    /* An enrolled certificate belongs to whoever issued it, not to us: never
       regenerate over it, and in particular do not let an mDNS rename -- which
       is what the hostname file below exists to catch -- throw it away. */
    f=fopen(mark_path,"r");if(f){fclose(f);f=fopen(key_path,"r");if(f){fclose(f);f=fopen(cert_path,"r");if(f){fclose(f);return 0;}}}
    f=fopen(key_path,"r"); if(f){fclose(f);f=fopen(cert_path,"r");if(f){fclose(f);if(!joan_read_file(host_path,&saved,&saved_len,sizeof(dns))&&saved_len==strlen(dns)+1&&!memcmp(saved,dns,strlen(dns))&&saved[saved_len-1]=='\n'){free(saved);return 0;}free(saved);saved=NULL;}}
    mbedtls_entropy_init(&entropy);mbedtls_ctr_drbg_init(&drbg);mbedtls_pk_init(&key);mbedtls_x509write_crt_init(&crt);mbedtls_mpi_init(&serial);
    if(mbedtls_ctr_drbg_seed(&drbg,mbedtls_entropy_func,&entropy,(const unsigned char*)personal,strlen(personal)))goto done;
    if(mbedtls_pk_setup(&key,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)))goto done;
    if(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(key),mbedtls_ctr_drbg_random,&drbg))goto done;
    memset(key_pem,0,sizeof(key_pem));if(mbedtls_pk_write_key_pem(&key,key_pem,sizeof(key_pem)))goto done;
    if(joan_random(serial_bytes,sizeof(serial_bytes))||mbedtls_mpi_read_binary(&serial,serial_bytes,sizeof(serial_bytes)))goto done;
    mbedtls_x509write_crt_set_version(&crt,MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt,MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt,&key);mbedtls_x509write_crt_set_issuer_key(&crt,&key);
    dns_len=strlen(dns);if(dns_len>63)goto done;
    san[0]=0x30;san[1]=(unsigned char)(dns_len+2);san[2]=0x82;san[3]=(unsigned char)dns_len;memcpy(san+4,dns,dns_len);san_len=dns_len+4;
    if(mbedtls_x509write_crt_set_subject_name(&crt,"CN=Jooan Local Camera,O=Local Device")||
       mbedtls_x509write_crt_set_issuer_name(&crt,"CN=Jooan Local Camera,O=Local Device")||
       mbedtls_x509write_crt_set_serial(&crt,&serial)||
       mbedtls_x509write_crt_set_validity(&crt,"20250101000000","20450101000000")||
       mbedtls_x509write_crt_set_basic_constraints(&crt,0,-1)||
       mbedtls_x509write_crt_set_key_usage(&crt,MBEDTLS_X509_KU_DIGITAL_SIGNATURE|MBEDTLS_X509_KU_KEY_AGREEMENT)||
       mbedtls_x509write_crt_set_extension(&crt,MBEDTLS_OID_SUBJECT_ALT_NAME,
           MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME),0,san,san_len))goto done;
    memset(cert_pem,0,sizeof(cert_pem));
    if(mbedtls_x509write_crt_pem(&crt,cert_pem,sizeof(cert_pem),mbedtls_ctr_drbg_random,&drbg))goto done;
    {char host_line[82];int hn=snprintf(host_line,sizeof(host_line),"%s\n",dns);if(hn<=0||
     joan_write_atomic(key_path,key_pem,strlen((char*)key_pem),0600)||joan_write_atomic(cert_path,cert_pem,strlen((char*)cert_pem),0644)||joan_write_atomic(host_path,host_line,(size_t)hn,0600))goto done;}
    rc=0;
done:
    memset(key_pem,0,sizeof(key_pem));mbedtls_mpi_free(&serial);mbedtls_x509write_crt_free(&crt);mbedtls_pk_free(&key);mbedtls_ctr_drbg_free(&drbg);mbedtls_entropy_free(&entropy);return rc;
}

/* Install an externally issued certificate.
 *
 * The point of this is the family's phones and the television: a self-signed
 * identity cannot be trusted by them at all -- it is CA:FALSE, so it cannot be
 * imported as an authority -- so the only way to a warning-free page is a
 * certificate from an authority they already trust. The camera has no route
 * off the LAN, which rules out an ACME HTTP challenge, but says nothing about
 * a DNS challenge solved elsewhere and the result handed to us here.
 *
 * Takes one PEM bundle: the private key and the certificate chain in any
 * order, which is what every ACME client can already emit.
 */
static const char *pem_block_end(const char *from,const char *limit)
{
    const char *e=memmem(from,(size_t)(limit-from),"-----END ",9);
    if(!e)return NULL;
    e=memchr(e,'\n',(size_t)(limit-e));
    return e?e+1:limit;
}

int joan_tls_enroll_identity(const JoanConfig *cfg,const char *pem,size_t len,char *why,size_t why_len)
{
    char key_path[512],cert_path[512],mark_path[512];
    char *keybuf=NULL,*certbuf=NULL; size_t keylen=0,certlen=0;
    const char *p=pem,*limit=pem+len; int rc=-1;
    mbedtls_x509_crt chain; mbedtls_pk_context pk;
    mbedtls_x509_crt_init(&chain); mbedtls_pk_init(&pk);
    if(!len||len>JOAN_MAX_BODY){snprintf(why,why_len,"empty or oversized bundle");goto out;}
    if(!(keybuf=malloc(len+1))||!(certbuf=malloc(len+1))){snprintf(why,why_len,"out of memory");goto out;}
    /* Split the bundle by PEM block, so the key and the chain can arrive in
       either order and an unexpected block is ignored rather than fatal. */
    while((p=memmem(p,(size_t)(limit-p),"-----BEGIN ",11))){
        const char *end=pem_block_end(p,limit);
        size_t block;
        if(!end){snprintf(why,why_len,"truncated PEM block");goto out;}
        block=(size_t)(end-p);
        if(memmem(p,block<64?block:64,"PRIVATE KEY",11)){memcpy(keybuf+keylen,p,block);keylen+=block;}
        else if(memmem(p,block<64?block:64,"CERTIFICATE",11)){memcpy(certbuf+certlen,p,block);certlen+=block;}
        p=end;
    }
    keybuf[keylen]=0; certbuf[certlen]=0;
    if(!keylen){snprintf(why,why_len,"no private key in the bundle");goto out;}
    if(!certlen){snprintf(why,why_len,"no certificate in the bundle");goto out;}
    if(mbedtls_x509_crt_parse(&chain,(const unsigned char*)certbuf,certlen+1)){snprintf(why,why_len,"certificate did not parse");goto out;}
    if(mbedtls_pk_parse_key(&pk,(const unsigned char*)keybuf,keylen+1,NULL,0)){snprintf(why,why_len,"private key did not parse, or is encrypted");goto out;}
    if(mbedtls_pk_check_pair(&chain.pk,&pk)){snprintf(why,why_len,"the key does not match the certificate");goto out;}
    /* Dates are checked against the camera's clock, which has no RTC. Say so,
       because "expired" here is as likely to mean the clock is wrong. */
    if(mbedtls_x509_time_is_past(&chain.valid_to)){snprintf(why,why_len,"certificate is expired, or the camera clock is wrong");goto out;}
    if(mbedtls_x509_time_is_future(&chain.valid_from)){snprintf(why,why_len,"certificate is not valid yet, or the camera clock is wrong");goto out;}
    snprintf(key_path,sizeof(key_path),"%s/tls-key.pem",cfg->state_dir);
    snprintf(cert_path,sizeof(cert_path),"%s/tls-cert.pem",cfg->state_dir);
    snprintf(mark_path,sizeof(mark_path),"%s/tls-enrolled",cfg->state_dir);
    if(joan_write_atomic(key_path,keybuf,keylen,0600)||
       joan_write_atomic(cert_path,certbuf,certlen,0644)||
       joan_write_atomic(mark_path,"enrolled\n",9,0600)){snprintf(why,why_len,"could not store the identity");goto out;}
    rc=0;
out:
    if(keybuf)memset(keybuf,0,len);
    free(keybuf);free(certbuf);
    mbedtls_pk_free(&pk);mbedtls_x509_crt_free(&chain);
    return rc;
}

/* Back to a generated identity: drop the files and let ensure() rebuild. */
int joan_tls_clear_identity(const JoanConfig *cfg)
{
    char path[512];
    snprintf(path,sizeof(path),"%s/tls-enrolled",cfg->state_dir);remove(path);
    snprintf(path,sizeof(path),"%s/tls-key.pem",cfg->state_dir);remove(path);
    snprintf(path,sizeof(path),"%s/tls-cert.pem",cfg->state_dir);remove(path);
    return joan_tls_ensure_identity(cfg);
}
#endif
