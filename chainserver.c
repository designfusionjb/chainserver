/*
 * chainserver.c
 * Work-in-Progress ...
 *
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <getdns/getdns.h>
#include <getdns/getdns_extra.h>
#include <getdns/getdns_ext_libevent.h>

#include "utils.h"


/*
 * DNSSEC Authentication Chain TLS extension type value
 */

#define DNSSEC_CHAIN_EXT_TYPE 53


/*
 * Server certificate and key files (PEM format) - hardcoded defaults.
 */

#define SERVER_CERT "server.crt"
#define SERVER_KEY  "server.key"


/*
 * Enumerated Types and Global variables
 */

enum AUTH_MODE { 
    MODE_BOTH=0, 
    MODE_DANE, 
    MODE_PKIX 
};

int debug = 0;
uint16_t port;
enum AUTH_MODE auth_mode = MODE_BOTH;
char *service_name = NULL;

char *server_name = NULL;
char *certfile = SERVER_CERT;
char *keyfile = SERVER_KEY;
char *CAfile = NULL;
int clientauth = 0;
int dnssec_chain = 1;
unsigned char *dnssec_chain_data = NULL;


/*
 * usage(): Print usage string and exit.
 */

void print_usage(const char *progname)
{
    fprintf(stdout, "\nUsage: %s [options] <portnumber>\n\n"
            "       -h:               print this help message\n"
            "       -d:               debug mode\n"
	    "       -sname <name>:    server name\n"
	    "       -cert <file>:     server certificate file\n"
	    "       -key <file>:      server private key file\n"
	    "       -clientauth:      require client authentication\n"
	    "       -CAfile <file>:   CA file for client authentication\n"
	    "\n",
	    progname);
    exit(1);
}


/*
 * parse_options()
 */

void parse_options(const char *progname, int argc, char **argv)
{
    int i;
    char *optword;

    for (i = 1; i < argc; i++) {

	optword = argv[i];

	if (!strcmp(optword, "-h")) {
	    print_usage(progname);
	} else if (!strcmp(optword, "-d")) {
	    debug = 1;
	} else if (!strcmp(optword, "-sname")) {
	    if (++i >= argc || !*argv[i]) {
		fprintf(stderr, "-sname: server name.\n");
		print_usage(progname);
	    }
	    server_name = argv[i];
	} else if (!strcmp(optword, "-cert")) {
	    if (++i >= argc || !*argv[i]) {
		fprintf(stderr, "-cert: certificate file expected.\n");
		print_usage(progname);
	    }
	    certfile = argv[i];
	} else if (!strcmp(optword, "-key")) {
	    if (++i >= argc || !*argv[i]) {
		fprintf(stderr, "-key: private key file expected.\n");
		print_usage(progname);
	    }
	    keyfile = argv[i];
	} else if (!strcmp(optword, "-CAfile")) {
	    if (++i >= argc || !*argv[i]) {
		fprintf(stderr, "-CAfile: CA file expected.\n");
		print_usage(progname);
	    }
	    CAfile = argv[i];
	} else if (!strcmp(optword, "-clientauth")) {
	    clientauth = 1;
	} else if (optword[0] == '-') {
	    fprintf(stderr, "Unrecognized option: %s\n", optword);
	    print_usage(progname);
	} else {
	    break;
	}

    }

    if (optword == NULL) {
	fprintf(stderr, "Error: no port number specified.\n");
	print_usage(progname);
    } else if ((argc - i) != 1) {
	fprintf(stderr, "Error: too many arguments.\n");
	print_usage(progname);
    }

    port = atoi(optword);

    if (!server_name) {
	server_name = malloc(512);
	gethostname(server_name, 512);
    }

    return;
}


/*
 * Linked List of Wire format RRs and associated routines.
 */


typedef struct wirerr {
    getdns_bindata *node;
    struct wirerr *next;
} wirerr;

wirerr *wirerr_list = NULL;
size_t wirerr_count = 0;
size_t wirerr_size = 0;

wirerr *insert_wirerr(wirerr *current, getdns_bindata *new)
{
    wirerr *w = malloc(sizeof(wirerr));
    w->node = new;
    w->next = NULL;
    wirerr_count++;
    wirerr_size += new->size;

    if (current == NULL) {
        wirerr_list = w;
    } else
        current->next = w;
    return w;
}

void free_wirerr_list(wirerr *head)
{
    wirerr *current;
    while ((current = head) != NULL) {
        head = head->next;
        free(current->node->data);
	free(current->node);
        free(current);
    }
    return;
}


/*
 * getchain(): get DNSSEC chain data for given qname, qtype
 */

getdns_bindata *getchain(char *qname, uint16_t qtype)
{
    unsigned char *cp;
    uint32_t status;
    getdns_context *ctx = NULL;
    getdns_return_t rc;
    getdns_dict    *extensions = NULL;
    getdns_dict *response;
    getdns_bindata *chaindata = malloc(sizeof(getdns_bindata));
    wirerr *wp = wirerr_list;

    rc = getdns_context_create(&ctx, 1);
    if (rc != GETDNS_RETURN_GOOD) {
	(void) fprintf(stderr, "Context creation failed: %d", rc);
	return NULL;
    }

    if (! (extensions = getdns_dict_create())) {
	fprintf(stderr, "FAIL: Error creating extensions dict\n");
	return NULL;
    }

    if ((rc = getdns_dict_set_int(extensions, "dnssec_return_only_secure",
				  GETDNS_EXTENSION_TRUE))) {
	fprintf(stderr, "FAIL: setting dnssec_return_only_secure: %s\n",
		getdns_get_errorstr_by_id(rc));
	return NULL;
    }

    if ((rc = getdns_dict_set_int(extensions, "dnssec_return_validation_chain",
				  GETDNS_EXTENSION_TRUE))) {
	fprintf(stderr, "FAIL: setting +dnssec_return_validation_chain: %s\n",
		getdns_get_errorstr_by_id(rc));
	return NULL;
    }

    rc = getdns_general_sync(ctx, qname, qtype, extensions, &response);
    if (rc != GETDNS_RETURN_GOOD) {
	(void) fprintf(stderr, "getdns_general() failed, rc=%d, %s\n", 
		       rc, getdns_get_errorstr_by_id(rc));
	getdns_context_destroy(ctx);
	return NULL;
    }

    (void) getdns_dict_get_int(response, "status", &status);

    switch (status) {
    case GETDNS_RESPSTATUS_GOOD:
	break;
    case GETDNS_RESPSTATUS_NO_NAME:
	fprintf(stderr, "FAIL: %s: Non existent domain name.\n", qname);
	return NULL;
    case GETDNS_RESPSTATUS_ALL_TIMEOUT:
	fprintf(stderr, "FAIL: %s: Query timed out.\n", qname);
	return NULL;
    case GETDNS_RESPSTATUS_NO_SECURE_ANSWERS:
	fprintf(stderr, "%s: Insecure address records.\n", qname);
	return NULL;
    case GETDNS_RESPSTATUS_ALL_BOGUS_ANSWERS:
	fprintf(stderr, "FAIL: %s: All bogus answers.\n", qname);
	return NULL;
    default:
        fprintf(stderr, "FAIL: %s: error status code: %d.\n", qname, status);
        return NULL;
    }

    getdns_list *replies_tree;
    rc = getdns_dict_get_list(response, "replies_tree", &replies_tree);
    if (rc != GETDNS_RETURN_GOOD) {
	(void) fprintf(stdout, "dict_get_list: replies_tree: rc=%d\n", rc);
	return NULL;
    }

    size_t reply_count;
    (void) getdns_list_get_length(replies_tree, &reply_count);

    size_t i;
    for ( i = 0; i < reply_count; i++ ) {

	getdns_dict *reply;
	getdns_list *answer;
	size_t rr_count;
	size_t j;

	(void) getdns_list_get_dict(replies_tree, i, &reply);
	(void) getdns_dict_get_list(reply, "answer", &answer);
	(void) getdns_list_get_length(answer, &rr_count);

	if (rr_count == 0) {
	    (void) fprintf(stderr, "FAIL: %s: NODATA response.\n", qname);
	    return NULL;
	}

	for ( j = 0; j < rr_count; j++ ) {
	    getdns_dict *rr = NULL;
	    getdns_bindata *wire = malloc(sizeof(getdns_bindata));
	    (void) getdns_list_get_dict(answer, j, &rr);
	    rc = getdns_rr_dict2wire(rr, &wire->data, &wire->size);
            if (rc != GETDNS_RETURN_GOOD) {
		(void) fprintf(stderr, "rrdict2wire() failed: %d\n", rc);
                return NULL;
            }
	    wp = insert_wirerr(wp, wire);
	}

    }

    getdns_list *val_chain;
    rc = getdns_dict_get_list(response, "validation_chain", &val_chain);
    if (rc != GETDNS_RETURN_GOOD) {
        (void) fprintf(stderr, "FAIL: getting validation_chain: rc=%d\n", rc);
	return NULL;
    }

    size_t rr_count;
    (void) getdns_list_get_length(val_chain, &rr_count);

    for ( i = 0; i < rr_count; i++ ) {	
	getdns_dict *rr = NULL;
        getdns_bindata *wire = malloc(sizeof(getdns_bindata));
	(void) getdns_list_get_dict(val_chain, i, &rr);
	rc = getdns_rr_dict2wire(rr, &wire->data, &wire->size);
        if (rc != GETDNS_RETURN_GOOD) {
            (void) fprintf(stderr, "rrdict2wire() failed: %d\n", rc);
            return NULL;
        }
	wp = insert_wirerr(wp, wire);
    }

    getdns_context_destroy(ctx);

    /* 
     * Generate dnssec_chain extension data and return pointer to it.
     */
    chaindata->size = 4 + wirerr_size;
    chaindata->data = malloc(chaindata->size);

    cp = chaindata->data;
    *(cp + 0) = (DNSSEC_CHAIN_EXT_TYPE >> 8) & 0xff;  /* Extension Type */
    *(cp + 1) = (DNSSEC_CHAIN_EXT_TYPE) & 0xff;
    *(cp + 2) = (wirerr_size >> 8) & 0xff;            /* Extension (data) Size */
    *(cp + 3) = (wirerr_size) & 0xff;

    cp = chaindata->data + 4;

    for (wp = wirerr_list; wp != NULL; wp = wp->next) {
	getdns_bindata *g = wp->node;
	(void) memcpy(cp, g->data, g->size);
	cp += g->size;
    }

    fprintf(stdout, "\nDEBUG (REMOVE ME) chaindata:\n%s\n\n", 
	    bindata2hexstring(chaindata));

    return chaindata;
}


/*
 * main(): DANE chainserver program
 */

int main(int argc, char **argv)
{

    const char *progname;
    char ipstring[INET6_ADDRSTRLEN];
    char tlsa_name[512];
    getdns_bindata *chaindata = NULL;
    int return_status = 1;                    /* program return status */
    int sock;
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    const SSL_CIPHER *cipher = NULL;
    X509_VERIFY_PARAM *vpm = NULL;
    BIO *sbio;

    /* uint8_t usage, selector, mtype; */

    if ((progname = strrchr(argv[0], '/')))
        progname++;
    else
        progname = argv[0];

    parse_options(progname, argc, argv);

    /*
     * Query my TLSA record and build DNSSEC chain data for it.
     */

    snprintf(tlsa_name, 512, "_%d._tcp.%s", port, server_name);
    chaindata = getchain(tlsa_name, GETDNS_RRTYPE_TLSA);
    if (chaindata) {
	fprintf(stdout, "Got DNSSEC chain data for %s, size=%zu octets\n", 
		tlsa_name, chaindata->size - 4);
    } else {
	fprintf(stdout, "Failed to get DNSSEC chain data for %s\n", tlsa_name);
	/* return 1; */
    }

    /*
     * Initialize OpenSSL TLS library context, certificate authority
     * stores, and hostname verification parameters.
     */

    SSL_load_error_strings();
    SSL_library_init();

    ctx = SSL_CTX_new(TLS_server_method());
    (void) SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);

    if (!CAfile) {
	if (!SSL_CTX_set_default_verify_paths(ctx)) {
	    fprintf(stderr, "Failed to load default certificate authorities.\n");
	    ERR_print_errors_fp(stderr);
	    goto cleanup;
	}
    } else {
	if (!SSL_CTX_load_verify_locations(ctx, CAfile, NULL)) {
	    fprintf(stderr, "Failed to load certificate authority store: %s.\n",
		    CAfile);
	    ERR_print_errors_fp(stderr);
	    goto cleanup;
	}
    }

    /* SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL); */
    SSL_CTX_set_verify_depth(ctx, 10);

    /* 
     * Read my server certificate and private key
     */

    if (!SSL_CTX_use_certificate_file(ctx, SERVER_CERT, SSL_FILETYPE_PEM)) {
	fprintf(stderr, "Failed to load server certificate.\n");
	ERR_print_errors_fp(stderr);
	goto cleanup;
    }

    if (!SSL_CTX_use_PrivateKey_file(ctx, SERVER_KEY, SSL_FILETYPE_PEM)) {
	fprintf(stderr, "Failed to load server private key.\n");
	ERR_print_errors_fp(stderr);
	goto cleanup;
    }   

    /*
     * load dnssec_chain Server Hello extensions into context
     */

    if (chaindata)
	if (!SSL_CTX_use_serverinfo(ctx, chaindata->data, chaindata->size)) {
	    fprintf(stderr, "failed loading dnssec_chain_data extension.\n");
	    goto cleanup;
	}

    /*
     * Setup session resumption capability
     */

    const unsigned char *sid_ctx = (const unsigned char *) "chainserver";
    (void) SSL_CTX_set_session_id_context(ctx, sid_ctx, sizeof(sid_ctx));

    /*
     * Setup listening socket
     */

    struct sockaddr_in serv_addr, cli_addr;
    socklen_t cli_addr_len = sizeof(struct sockaddr_in);
    memset(&serv_addr, 0, sizeof(serv_addr));
    memset(&cli_addr, 0, sizeof(cli_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((uint16_t) port);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == -1) {
	perror("socket");
	goto cleanup;
    }

    int j = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *) &j, sizeof j);
    if (bind(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) == -1) {
	perror("bind");
	fprintf(stderr, "Unable to bind to server address.\n");
	goto cleanup;
    }

    if (listen(sock, 5) == -1) {
	perror("listen");
	close(sock);
	goto cleanup;
    }
    fprintf(stdout, "Server listening on port %d\n", port);

    int clisock;

    while (1) {

	pid_t pid;
	if ((clisock = accept(sock, (struct sockaddr *) &cli_addr, 
			      &cli_addr_len)) < 0) {
	    perror("accept");
	    fprintf(stderr, "Error accepting client socket.\n");
	    goto cleanup;
	}

	if ((pid = fork()) == -1) {
	    perror("fork");
	    fprintf(stderr, "Error: fork() failed.\n");
	    /* should we abort here instead? */
	    close(clisock);
	    continue;
	} else if (pid != 0) {
	    /* parent process */
	    close(clisock);
	    continue;
	}

	/* child process */
	inet_ntop(AF_INET, &cli_addr.sin_addr, ipstring, sizeof ipstring);
	fprintf(stdout, "Connection from %s port=%d\n", 
		ipstring, cli_addr.sin_port);

	ssl = SSL_new(ctx);
	if (! ssl) {
	    fprintf(stderr, "SSL_new() failed.\n");
	    ERR_print_errors_fp(stderr);
	    close(sock);
	    continue;
	}

	/* No partial label wildcards */
	SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);

	/* Set connect mode (client) and tie socket to TLS context */
	sbio = BIO_new_socket(clisock, BIO_NOCLOSE);
	SSL_set_bio(ssl, sbio, sbio);

	/* Perform TLS connection handshake & peer authentication */
	if (SSL_accept(ssl) <= 0) {
	    fprintf(stderr, "TLS connection failed.\n");
	    ERR_print_errors_fp(stderr);
	    SSL_free(ssl);
	    close(sock);
	    continue;
	}

	cipher = SSL_get_current_cipher(ssl);
	fprintf(stdout, "%s Cipher: %s %s\n\n", SSL_get_version(ssl),
		SSL_CIPHER_get_version(cipher), SSL_CIPHER_get_name(cipher));

	/* TODO: read HTTP request and spit out a response */
	sleep(2);

	/* Shutdown */
	SSL_shutdown(ssl);
	SSL_free(ssl);
	close(clisock);
    }
    close(sock);

cleanup:
    free(dnssec_chain_data);
    if (ctx) {
	X509_VERIFY_PARAM_free(vpm);
	SSL_CTX_free(ctx);
    }

    /* Returns 0 if at least one SSL peer authenticates */
    return return_status;
}
