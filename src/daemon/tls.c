#include "joan_daemon.h"

#include <stdio.h>
#include <string.h>

#ifdef JOAN_NO_TLS
int joan_tls_ensure_identity(const JoanConfig *cfg) { (void)cfg; return -1; }
#else
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

int joan_tls_ensure_identity(const JoanConfig *cfg)
{
    char key_path[512], cert_path[512]; FILE *f;
    mbedtls_entropy_context entropy; mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context key; mbedtls_x509write_cert crt; mbedtls_mpi serial;
    unsigned char key_pem[2048], cert_pem[4096], serial_bytes[16];
    const char *personal="joan-local-device-identity"; int rc=-1;
    snprintf(key_path,sizeof(key_path),"%s/tls-key.pem",cfg->state_dir);
    snprintf(cert_path,sizeof(cert_path),"%s/tls-cert.pem",cfg->state_dir);
    f=fopen(key_path,"r"); if(f){fclose(f);f=fopen(cert_path,"r");if(f){fclose(f);return 0;}}
    mbedtls_entropy_init(&entropy);mbedtls_ctr_drbg_init(&drbg);mbedtls_pk_init(&key);mbedtls_x509write_crt_init(&crt);mbedtls_mpi_init(&serial);
    if(mbedtls_ctr_drbg_seed(&drbg,mbedtls_entropy_func,&entropy,(const unsigned char*)personal,strlen(personal)))goto done;
    if(mbedtls_pk_setup(&key,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)))goto done;
    if(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(key),mbedtls_ctr_drbg_random,&drbg))goto done;
    memset(key_pem,0,sizeof(key_pem));if(mbedtls_pk_write_key_pem(&key,key_pem,sizeof(key_pem)))goto done;
    if(joan_random(serial_bytes,sizeof(serial_bytes))||mbedtls_mpi_read_binary(&serial,serial_bytes,sizeof(serial_bytes)))goto done;
    mbedtls_x509write_crt_set_version(&crt,MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt,MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt,&key);mbedtls_x509write_crt_set_issuer_key(&crt,&key);
    if(mbedtls_x509write_crt_set_subject_name(&crt,"CN=Jooan Local Camera,O=Local Device")||
       mbedtls_x509write_crt_set_issuer_name(&crt,"CN=Jooan Local Camera,O=Local Device")||
       mbedtls_x509write_crt_set_serial(&crt,&serial)||
       mbedtls_x509write_crt_set_validity(&crt,"20250101000000","20450101000000")||
       mbedtls_x509write_crt_set_basic_constraints(&crt,0,-1)||
       mbedtls_x509write_crt_set_key_usage(&crt,MBEDTLS_X509_KU_DIGITAL_SIGNATURE|MBEDTLS_X509_KU_KEY_AGREEMENT))goto done;
    memset(cert_pem,0,sizeof(cert_pem));
    if(mbedtls_x509write_crt_pem(&crt,cert_pem,sizeof(cert_pem),mbedtls_ctr_drbg_random,&drbg))goto done;
    if(joan_write_atomic(key_path,key_pem,strlen((char*)key_pem),0600)||joan_write_atomic(cert_path,cert_pem,strlen((char*)cert_pem),0644))goto done;
    rc=0;
done:
    memset(key_pem,0,sizeof(key_pem));mbedtls_mpi_free(&serial);mbedtls_x509write_crt_free(&crt);mbedtls_pk_free(&key);mbedtls_ctr_drbg_free(&drbg);mbedtls_entropy_free(&entropy);return rc;
}
#endif
