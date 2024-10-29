
#include <ssl.h>

#include <stdio.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "niceties.h"

struct ssl
{
  int listen_sd;
  int sd;
  struct sockaddr_in sa_serv;
  struct sockaddr_in sa_cli;
  socklen_t client_len;
  SSL_CTX* ctx;
  SSL*     ssl;
  X509*    client_cert;
  char*    str;
};

void ssl_init()
{
  SSL_load_error_strings();
  SSLeay_add_ssl_algorithms();
}

static ssl_t ssl1_instance;

static void ssl_error(SSL *ssl, int err) {
  int ssl_err = SSL_get_error(ssl, err);

  printf("Error: SSL_accept() error code %d\n", ssl_err);
  if (ssl_err == SSL_ERROR_SYSCALL) {
    if (err == -1) printf("  I/O error (%s)\n", strerror(errno));
    if (err == 0) printf("  SSL peer closed connection\n");
  }
}

ssl_t* ssl_new()
{
    int port = 8234;
    int err;

    ssl_t* ssl1 = &ssl1_instance;
    
    const SSL_METHOD *meth = SSLv23_server_method();
    ssl1->ctx = SSL_CTX_new(meth);
    if (ssl1->ctx == NULL) {
        return NULL;
    }

#define CERTF "/home/ewertons/srv-cert.pem"
#define KEYF  "/home/ewertons/srv-priv.pem"

if (SSL_CTX_use_certificate_file(ssl1->ctx, CERTF, SSL_FILETYPE_PEM) <= 0) {
    printf("Error: Valid server certificate not found in " CERTF "\n");
    return NULL;
  }
  if (SSL_CTX_use_PrivateKey_file(ssl1->ctx, KEYF, SSL_FILETYPE_PEM) <= 0) {
    printf("Error: Valid server private key not found in " CERTF "\n");
    return NULL;
  }

  if (!SSL_CTX_check_private_key(ssl1->ctx)) {
    printf("Error: Private key does not match the certificate public key\n");
    return NULL;
  }

  ssl1->listen_sd = socket(AF_INET, SOCK_STREAM, 0);
  if (ssl1->listen_sd == -1) return NULL;
  
  memset(&ssl1->sa_serv, '\0', sizeof(ssl1->sa_serv));
  ssl1->sa_serv.sin_family      = AF_INET;
  ssl1->sa_serv.sin_addr.s_addr = INADDR_ANY;
  ssl1->sa_serv.sin_port        = htons(port);          /* Server Port number */
  
  err = bind(ssl1->listen_sd, (struct sockaddr*) &ssl1->sa_serv, sizeof (ssl1->sa_serv));
  if (err == -1) {
    printf("Error: Could not bind to port %d (%s)\n", port, strerror(errno));
    return NULL;
  }
	     
  err = listen(ssl1->listen_sd, 5);
  if (err == -1) {
    printf("Error: listen() (%s)\n", strerror(errno));
    return NULL;
  }

  ssl1->client_len = sizeof(ssl1->sa_cli);
  ssl1->sd = accept(ssl1->listen_sd, (struct sockaddr*)&ssl1->sa_cli, &ssl1->client_len);
  if (ssl1->sd == -1) {
    printf("Error: accept() (%s)\n", strerror(errno));
    return NULL;
  }
  close(ssl1->listen_sd);

  {
    char buffer[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ssl1->sa_cli.sin_addr.s_addr, buffer, sizeof(buffer));
    printf("Connection from %s:%d\n", buffer, ntohs(ssl1->sa_cli.sin_port));
  }
  
  /* ----------------------------------------------- */
  /* TCP connection is ready. Do server side SSL. */

  ssl1->ssl = SSL_new(ssl1->ctx);
  SSL_set_fd(ssl1->ssl, ssl1->sd);
  err = SSL_accept(ssl1->ssl);
  if (err != 1) {
    ssl_error(ssl1->ssl, err);
    return NULL;
  }

  printf ("SSL connection using %s\n", SSL_get_cipher (ssl1->ssl));
  
  /* Get client's certificate */

  ssl1->client_cert = SSL_get_peer_certificate (ssl1->ssl);
  if (ssl1->client_cert != NULL) {
    printf ("Client certificate:\n");
    
    ssl1->str = X509_NAME_oneline (X509_get_subject_name (ssl1->client_cert), 0, 0);
    if (!ssl1->str) return NULL;
    printf ("\t subject: %s\n", ssl1->str);
    OPENSSL_free (ssl1->str);
    
    ssl1->str = X509_NAME_oneline (X509_get_issuer_name  (ssl1->client_cert), 0, 0);
    if (!ssl1->str) return NULL;
    printf ("\t issuer: %s\n", ssl1->str);
    OPENSSL_free (ssl1->str);
    
    /* We could do all sorts of certificate verification stuff here before
       deallocating the certificate. */
    
    X509_free (ssl1->client_cert);
  } else {
    printf ("Client does not have certificate.\n");
  }

    return ssl1;
}

void ssl_accept()
{

}

int ssl_read(ssl_t* ssl1, span_t buffer, span_t* out_read)
{
    int result;

    if (ssl1 == NULL)
    {
        result = ERROR;
    }
    else
    {
        int bytes_read;

        bytes_read = SSL_read(ssl1->ssl, span_get_ptr(buffer), span_get_size(buffer));

        if (bytes_read > 0)
        {
            *out_read = span_slice(buffer, 0, bytes_read);
            result = OK;
        }
        else
        {
            int reason = SSL_get_error(ssl1->ssl, bytes_read);

            switch(reason)
            {
                case SSL_ERROR_ZERO_RETURN:
                case SSL_ERROR_SYSCALL:
                case SSL_ERROR_SSL:
                    result = ERROR;
                    break;
                default:
                    result = OK;
                    break;
            };
        }
    }

    return result;
}

int ssl_write(ssl_t* ssl1, span_t data)
{
    int result;

    if (ssl1 == NULL)
    {
        result = ERROR;
    }
    else
    {
        result = OK;

        while (span_get_size(data) > 0)
        {
            int n = SSL_write(ssl1->ssl, span_get_ptr(data), span_get_size(data));

            if (n <= 0)
            {
                result = ERROR;
                break;
            }
            else
            {
                data = span_slice_to_end(data, n);
            }
        }
    }

    return result;
}