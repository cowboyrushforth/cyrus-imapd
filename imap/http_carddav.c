/* http_carddav.c -- Routines for handling CardDAV collections in httpd
 *
 * Copyright (c) 1994-2013 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
/*
 * TODO:
 *   Support <filter> for addressbook-query Report
 *
 */

#include <config.h>

#include <syslog.h>

#include <libical/vcc.h>
#include <libxml/tree.h>
#include <libxml/uri.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "acl.h"
#include "append.h"
#include "carddav_db.h"
#include "charset.h"
#include "global.h"
#include "hash.h"
#include "httpd.h"
#include "http_dav.h"
#include "http_err.h"
#include "http_proxy.h"
#include "imap_err.h"
#include "index.h"
#include "mailbox.h"
#include "mboxlist.h"
#include "message.h"
#include "message_guid.h"
#include "proxy.h"
#include "rfc822date.h"
#include "smtpclient.h"
#include "spool.h"
#include "stristr.h"
#include "util.h"
#include "version.h"
#include "xmalloc.h"
#include "xstrlcat.h"
#include "xstrlcpy.h"

static struct carddav_db *auth_carddavdb = NULL;

static struct carddav_db *my_carddav_open(struct mailbox *mailbox);
static void my_carddav_close(struct carddav_db *carddavdb);
static void my_carddav_init(struct buf *serverinfo);
static void my_carddav_auth(const char *userid);
static void my_carddav_reset(void);
static void my_carddav_shutdown(void);

static int carddav_parse_path(const char *path,
			      struct request_target_t *tgt, const char **errstr);

static int carddav_put(struct transaction_t *txn, VObject *vcard,
		       struct mailbox *mailbox, const char *resource,
		       struct carddav_db *davdb, unsigned flags);
static VObject *vcard_string_as_vobject(const char *str)
{
    return Parse_MIME(str, strlen(str));
}
static void free_vobject(VObject *vobj)
{
    cleanVObject(vobj);
    cleanStrTbl();
}

static int propfind_getcontenttype(const xmlChar *name, xmlNsPtr ns,
				   struct propfind_ctx *fctx, xmlNodePtr resp,
				   struct propstat propstat[], void *rock);
static int propfind_restype(const xmlChar *name, xmlNsPtr ns,
			    struct propfind_ctx *fctx, xmlNodePtr resp,
			    struct propstat propstat[], void *rock);
static int propfind_addrdata(const xmlChar *name, xmlNsPtr ns,
			     struct propfind_ctx *fctx, xmlNodePtr resp,
			     struct propstat propstat[], void *rock);
static int propfind_suppaddrdata(const xmlChar *name, xmlNsPtr ns,
				 struct propfind_ctx *fctx, xmlNodePtr resp,
				 struct propstat propstat[], void *rock);

static int report_card_query(struct transaction_t *txn,
			     struct meth_params *rparams,
			     xmlNodePtr inroot, struct propfind_ctx *fctx);

static struct mime_type_t carddav_mime_types[] = {
    /* First item MUST be the default type and storage format */
    { "text/vcard; charset=utf-8", "3.0", "vcf", NULL,
      (void * (*)(const char*)) &vcard_string_as_vobject,
      (void (*)(void *)) &free_vobject, NULL, NULL
    },
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

/* Array of supported REPORTs */
static const struct report_type_t carddav_reports[] = {

    /* WebDAV Versioning (RFC 3253) REPORTs */
    { "expand-property", NS_DAV, "multistatus", &report_expand_prop,
      DACL_READ, 0 },

    /* WebDAV ACL (RFC 3744) REPORTs */
    { "acl-principal-prop-set", NS_DAV, "multistatus", &report_acl_prin_prop,
      DACL_ADMIN, REPORT_NEED_MBOX | REPORT_DEPTH_ZERO },

    /* WebDAV Sync (RFC 6578) REPORTs */
    { "sync-collection", NS_DAV, "multistatus", &report_sync_col,
      DACL_READ, REPORT_NEED_MBOX | REPORT_NEED_PROPS },

    /* CardDAV (RFC 6352) REPORTs */
    { "addressbook-query", NS_CARDDAV, "multistatus", &report_card_query,
      DACL_READ, REPORT_NEED_MBOX | REPORT_ALLOW_PROPS },
    { "addressbook-multiget", NS_CARDDAV, "multistatus", &report_multiget,
      DACL_READ, REPORT_NEED_MBOX | REPORT_ALLOW_PROPS },

    { NULL, 0, NULL, NULL, 0, 0 }
};

/* Array of known "live" properties */
static const struct prop_entry carddav_props[] = {

    /* WebDAV (RFC 4918) properties */
    { "creationdate", NS_DAV,
      PROP_ALLPROP | PROP_COLLECTION | PROP_RESOURCE,
      propfind_creationdate, NULL, NULL },
    { "displayname", NS_DAV,
      PROP_ALLPROP | PROP_COLLECTION | PROP_RESOURCE,
      propfind_fromdb, proppatch_todb, NULL },
    { "getcontentlanguage", NS_DAV, PROP_ALLPROP | PROP_RESOURCE,
      propfind_fromhdr, NULL, "Content-Language" },
    { "getcontentlength", NS_DAV,
      PROP_ALLPROP | PROP_COLLECTION | PROP_RESOURCE,
      propfind_getlength, NULL, NULL },
    { "getcontenttype", NS_DAV,
      PROP_ALLPROP | PROP_COLLECTION | PROP_RESOURCE,
      propfind_getcontenttype, NULL, "Content-Type" },
    { "getetag", NS_DAV, PROP_ALLPROP | PROP_COLLECTION | PROP_RESOURCE,
      propfind_getetag, NULL, NULL },
    { "getlastmodified", NS_DAV,
      PROP_ALLPROP | PROP_COLLECTION | PROP_RESOURCE,
      propfind_getlastmod, NULL, NULL },
    { "lockdiscovery", NS_DAV, PROP_ALLPROP | PROP_RESOURCE,
      propfind_lockdisc, NULL, NULL },
    { "resourcetype", NS_DAV,
      PROP_ALLPROP | PROP_COLLECTION | PROP_RESOURCE,
      propfind_restype, proppatch_restype, "addressbook" },
    { "supportedlock", NS_DAV, PROP_ALLPROP | PROP_RESOURCE,
      propfind_suplock, NULL, NULL },

    /* WebDAV Versioning (RFC 3253) properties */
    { "supported-report-set", NS_DAV, PROP_COLLECTION,
      propfind_reportset, NULL, (void *) carddav_reports },

    /* WebDAV ACL (RFC 3744) properties */
    { "owner", NS_DAV, PROP_COLLECTION | PROP_RESOURCE | PROP_EXPAND,
      propfind_owner, NULL, NULL },
    { "group", NS_DAV, 0, NULL, NULL, NULL },
    { "supported-privilege-set", NS_DAV, PROP_COLLECTION | PROP_RESOURCE,
      propfind_supprivset, NULL, NULL },
    { "current-user-privilege-set", NS_DAV, PROP_COLLECTION | PROP_RESOURCE,
      propfind_curprivset, NULL, NULL },
    { "acl", NS_DAV, PROP_COLLECTION | PROP_RESOURCE,
      propfind_acl, NULL, NULL },
    { "acl-restrictions", NS_DAV, PROP_COLLECTION | PROP_RESOURCE,
      propfind_aclrestrict, NULL, NULL },
    { "inherited-acl-set", NS_DAV, 0, NULL, NULL, NULL },
    { "principal-collection-set", NS_DAV, PROP_COLLECTION | PROP_RESOURCE,
      propfind_princolset, NULL, NULL },

    /* WebDAV Quota (RFC 4331) properties */
    { "quota-available-bytes", NS_DAV, PROP_COLLECTION,
      propfind_quota, NULL, NULL },
    { "quota-used-bytes", NS_DAV, PROP_COLLECTION,
      propfind_quota, NULL, NULL },

    /* WebDAV Current Principal (RFC 5397) properties */
    { "current-user-principal", NS_DAV,
      PROP_COLLECTION | PROP_RESOURCE | PROP_EXPAND,
      propfind_curprin, NULL, NULL },

    /* WebDAV POST (RFC 5995) properties */
    { "add-member", NS_DAV, PROP_COLLECTION,
      NULL,  /* Until Apple Contacts is fixed */ NULL, NULL },

    /* WebDAV Sync (RFC 6578) properties */
    { "sync-token", NS_DAV, PROP_COLLECTION,
      propfind_sync_token, NULL, NULL },

    /* CardDAV (RFC 6352) properties */
    { "address-data", NS_CARDDAV,
      PROP_RESOURCE | PROP_PRESCREEN | PROP_NEEDPROP,
      propfind_addrdata, NULL, NULL },
    { "addressbook-description", NS_CARDDAV, PROP_COLLECTION,
      propfind_fromdb, proppatch_todb, NULL },
    { "supported-address-data", NS_CARDDAV, PROP_COLLECTION,
      propfind_suppaddrdata, NULL, NULL },
    { "max-resource-size", NS_CARDDAV, 0, NULL, NULL, NULL },

    /* Apple Calendar Server properties */
    { "getctag", NS_CS, PROP_ALLPROP | PROP_COLLECTION,
      propfind_sync_token, NULL, NULL },

    { NULL, 0, 0, NULL, NULL, NULL }
};

static struct meth_params carddav_params = {
    carddav_mime_types,
    &carddav_parse_path,
    &dav_check_precond,
    { (db_open_proc_t) &my_carddav_open,
      (db_close_proc_t) &my_carddav_close,
      (db_lookup_proc_t) &carddav_lookup_resource,
      (db_foreach_proc_t) &carddav_foreach,
      (db_write_proc_t) &carddav_write,
      (db_delete_proc_t) &carddav_delete,
      (db_delmbox_proc_t) &carddav_delmbox },
    NULL,					/* No ACL extensions */
    (put_proc_t) &carddav_put,
    NULL,		  	      		/* No special DELETE handling */
    NULL,		  	      		/* No special GET handling */
    { MBTYPE_ADDRESSBOOK, NULL, NULL, 0 },	/* No special MK* method */
    NULL,		  	      		/* No special POST handling */
    { CARDDAV_SUPP_DATA, (put_proc_t) &carddav_put },
    carddav_props,
    carddav_reports
};


/* Namespace for Carddav collections */
struct namespace_t namespace_addressbook = {
    URL_NS_ADDRESSBOOK, 0, "/dav/addressbooks", "/.well-known/carddav",
    1 /* auth */,
#if 0 /* Until Apple Contacts fixes their add-member implementation */
    (ALLOW_READ | ALLOW_POST | ALLOW_WRITE | ALLOW_DELETE |
     ALLOW_DAV | ALLOW_WRITECOL | ALLOW_CARD),
#else
    (ALLOW_READ | ALLOW_WRITE | ALLOW_DELETE |
     ALLOW_DAV | ALLOW_WRITECOL | ALLOW_CARD),
#endif
    &my_carddav_init, &my_carddav_auth, my_carddav_reset, &my_carddav_shutdown,
    { 
	{ &meth_acl,		&carddav_params },	/* ACL		*/
	{ &meth_copy,		&carddav_params },	/* COPY		*/
	{ &meth_delete,		&carddav_params },	/* DELETE	*/
	{ &meth_get_dav,	&carddav_params },	/* GET		*/
	{ &meth_get_dav,	&carddav_params },	/* HEAD		*/
	{ &meth_lock,		&carddav_params },	/* LOCK		*/
	{ NULL,			NULL },			/* MKCALENDAR	*/
	{ &meth_mkcol,		&carddav_params },	/* MKCOL	*/
	{ &meth_copy,		&carddav_params },	/* MOVE		*/
	{ &meth_options,	&carddav_parse_path },	/* OPTIONS	*/
#if 0 /* Until Apple Contacts fixes their add-member implementation */
	{ &meth_post,		&carddav_params },	/* POST		*/
#else
	{ NULL,			NULL },			/* POST		*/
#endif
	{ &meth_propfind,	&carddav_params },	/* PROPFIND	*/
	{ &meth_proppatch,	&carddav_params },	/* PROPPATCH	*/
	{ &meth_put,		&carddav_params },	/* PUT		*/
	{ &meth_report,		&carddav_params },	/* REPORT	*/
	{ &meth_trace,		&carddav_parse_path },	/* TRACE	*/
	{ &meth_unlock,		&carddav_params } 	/* UNLOCK	*/
    }
};


static struct carddav_db *my_carddav_open(struct mailbox *mailbox)
{
    if (httpd_userid && mboxname_userownsmailbox(httpd_userid, mailbox->name)) {
	return auth_carddavdb;
    }
    else {
	return carddav_open(mailbox, CARDDAV_CREATE);
    }
}


static void my_carddav_close(struct carddav_db *carddavdb)
{
    if (carddavdb && (carddavdb != auth_carddavdb)) carddav_close(carddavdb);
}


static void my_carddav_init(struct buf *serverinfo __attribute__((unused)))
{
    namespace_addressbook.enabled =
	config_httpmodules & IMAP_ENUM_HTTPMODULES_CARDDAV;

    if (!namespace_addressbook.enabled) return;

    if (!config_getstring(IMAPOPT_ADDRESSBOOKPREFIX)) {
	fatal("Required 'addressbookprefix' option is not set", EC_CONFIG);
    }

    carddav_init();

    namespace_principal.enabled = 1;
    namespace_principal.allow |= ALLOW_CARD;
}


#define DEFAULT_ADDRBOOK "Default"

static void my_carddav_auth(const char *userid)
{
    int r;
    size_t len;
    struct mboxlist_entry mbentry;
    char mailboxname[MAX_MAILBOX_BUFFER], rights[100], *partition = NULL;
    char ident[MAX_MAILBOX_NAME];
    struct buf acl = BUF_INITIALIZER;

    /* Construct mailbox name corresponding to userid's Inbox */
    (*httpd_namespace.mboxname_tointernal)(&httpd_namespace, "INBOX",
					   userid, mailboxname);
    len = strlen(mailboxname);

    if (httpd_userisadmin ||
	global_authisa(httpd_authstate, IMAPOPT_PROXYSERVERS)) {
	/* admin or proxy from frontend - won't have DAV database */
	return;
    }
    else if (config_mupdate_server && !config_getstring(IMAPOPT_PROXYSERVERS)) {
	/* proxy-only server - won't have DAV databases */
    }
    else {
	/* Open CardDAV DB for 'userid' */
	struct mailbox mailbox;

	mailbox.name = mailboxname;

	my_carddav_reset();
	auth_carddavdb = carddav_open(&mailbox, CARDDAV_CREATE);
	if (!auth_carddavdb) fatal("Unable to open CardDAV DB", EC_IOERR);
    }

    /* Auto-provision an addressbook for 'userid' */
    strlcpy(ident, userid, strcspn(userid, "@"));
    mboxname_hiersep_toexternal(&httpd_namespace, ident, 0);
    strlcpy(ident + strlen(ident), userid + strlen(ident), sizeof(ident) - strlen(ident));

    /* addressbook-home-set */
    len += snprintf(mailboxname+len, MAX_MAILBOX_BUFFER - len, ".%s",
		    config_getstring(IMAPOPT_ADDRESSBOOKPREFIX));
    r = mboxlist_lookup(mailboxname, &mbentry, NULL);
    if (r == IMAP_MAILBOX_NONEXISTENT) {
	if (config_mupdate_server) {
	    /* Find location of INBOX */
	    char inboxname[MAX_MAILBOX_BUFFER];

	    r = (*httpd_namespace.mboxname_tointernal)(&httpd_namespace,
						       "INBOX",
						       userid, inboxname);
	    if (!r) {
		char *server;

		r = http_mlookup(inboxname, &server, NULL, NULL);
		if (!r && server) {
		    proxy_findserver(server, &http_protocol, proxy_userid,
				     &backend_cached, NULL, NULL, httpd_in);

		    return;
		}
	    }
	}
	else r = 0;

	/* Create locally */
	if (!r) r = mboxlist_createmailboxcheck(mailboxname, 0, NULL, 0,
						userid, httpd_authstate, NULL,
						&partition, 0);
	if (!r) {
	    buf_reset(&acl);
	    cyrus_acl_masktostr(ACL_ALL, rights);
	    buf_printf(&acl, "%s\t%s\t", ident, rights);
	    r = mboxlist_createmailbox_full(mailboxname, MBTYPE_ADDRESSBOOK,
					    partition, 0,
					    userid, httpd_authstate,
					    OPT_POP3_NEW_UIDL, time(0),
					    buf_cstring(&acl), NULL,
					    0, 0, 0, NULL);
	}
	mbentry.partition = partition;
    }
    if (r) {
	if (partition) free(partition);
	buf_free(&acl);
	return;
    }

    /* Default addressbook */
    snprintf(mailboxname+len, MAX_MAILBOX_BUFFER - len, ".%s",
	     DEFAULT_ADDRBOOK);
    r = mboxlist_lookup(mailboxname, NULL, NULL);
    if (r == IMAP_MAILBOX_NONEXISTENT) {
	buf_reset(&acl);
	cyrus_acl_masktostr(ACL_ALL, rights);
	buf_printf(&acl, "%s\t%s\t", ident, rights);
	r = mboxlist_createmailbox_full(mailboxname, MBTYPE_ADDRESSBOOK,
					mbentry.partition, 0,
					userid, httpd_authstate,
					OPT_POP3_NEW_UIDL, time(0),
					buf_cstring(&acl), NULL,
					0, 0, 0, NULL);
    }

    if (partition) free(partition);
    buf_free(&acl);
}


static void my_carddav_reset(void)
{
    if (auth_carddavdb) carddav_close(auth_carddavdb);
    auth_carddavdb = NULL;
}


static void my_carddav_shutdown(void)
{
    carddav_done();
}


/* Parse request-target path in CardDAV namespace */
static int carddav_parse_path(const char *path,
			      struct request_target_t *tgt, const char **errstr)
{
    char *p;
    size_t len, siz;
    static const char *prefix = NULL;
    char userid[MAX_MAILBOX_BUFFER];
    char userdomain[MAX_MAILBOX_BUFFER];
    char *domain_start;
    int userlen, domainlen;

    /* Make a working copy of target path */
    strlcpy(tgt->path, path, sizeof(tgt->path));
    tgt->tail = tgt->path + strlen(tgt->path);

    p = tgt->path;

    /* Sanity check namespace */
    len = strlen(namespace_addressbook.prefix);
    if (strlen(p) < len ||
	strncmp(namespace_addressbook.prefix, p, len) ||
	(path[len] && path[len] != '/')) {
	*errstr = "Namespace mismatch request target path";
	return HTTP_FORBIDDEN;
    }

    /* Default to bare-bones Allow bits for toplevel collections */
    tgt->allow &= ~(ALLOW_POST|ALLOW_WRITE|ALLOW_DELETE);

    /* Skip namespace */
    p += len;
    if (!*p || !*++p) return 0;

    /* Check if we're in user space */
    len = strcspn(p, "/");
    if (!strncmp(p, "user", len)) {
	p += len;
	if (!*p || !*++p) return 0;

	/* Get user id */
	len = strcspn(p, "/");
	tgt->user = p;
	tgt->userlen = len;

	p += len;
	if (!*p || !*++p) goto done;

	len = strcspn(p, "/");
    }

    /* Get collection */
    tgt->collection = p;
    tgt->collen = len;

    p += len;
    if (!*p || !*++p) {
	/* Make sure collection is terminated with '/' */
	if (p[-1] != '/') *p++ = '/';
	goto done;
    }

    /* Get resource */
    len = strcspn(p, "/");
    tgt->resource = p;
    tgt->reslen = len;

    p += len;

    if (*p) {
//	*errstr = "Too many segments in request target path";
	return HTTP_NOT_FOUND;
    }

  done:
    /* Set proper Allow bits based on path components */
    if (tgt->collection) {
	if (tgt->resource) {
	    tgt->allow &= ~ALLOW_WRITECOL;
	    tgt->allow |= (ALLOW_WRITE|ALLOW_DELETE);
	}
#if 0 /* Until Apple Contacts fixes their add-member implementation */
	else tgt->allow |= (ALLOW_POST|ALLOW_DELETE);
#else
	else tgt->allow |= ALLOW_DELETE;
#endif
    }
    else if (tgt->user) tgt->allow |= ALLOW_DELETE;


    /* Create mailbox name from the parsed path */ 
    if (!prefix) prefix = config_getstring(IMAPOPT_ADDRESSBOOKPREFIX);

    p = tgt->mboxname;
    siz = MAX_MAILBOX_BUFFER;
    if (tgt->user) {
	if (tgt->userlen) {
	    domain_start = strchr(tgt->user, '@');
	    if (domain_start != NULL) {
		userlen = domain_start - tgt->user + 1;
		domain_start++;
		domainlen = tgt->userlen - userlen + 1;
	        strlcpy(userid, tgt->user, userlen);
	        mboxname_hiersep_tointernal(&httpd_namespace, userid, userlen);
		strlcpy(userdomain, domain_start, domainlen);
	        len = snprintf(p, siz, "%.*s!user.%.*s", (int) domainlen, userdomain, (int) userlen, userid);
            } else {
	        len = snprintf(p, siz, "user.%.*s", (int) tgt->userlen, tgt->user);
	        mboxname_hiersep_tointernal(&httpd_namespace, p+5, tgt->userlen);
            }
	    p += len;
	    siz -= len;
	}
    }

    len = snprintf(p, siz, "%s%s", p != tgt->mboxname ? "." : "", prefix);
    p += len;
    siz -= len;

    if (tgt->collection) {
	snprintf(p, siz, ".%.*s", (int) tgt->collen, tgt->collection);
    }

    return 0;
}

/* Perform a COPY/MOVE/PUT request
 *
 * preconditions:
 *   CARDDAV:valid-address-data
 *   CARDDAV:no-uid-conflict (DAV:href)
 *   CARDDAV:max-resource-size
 */
static int carddav_put(struct transaction_t *txn, VObject *vcard,
		       struct mailbox *mailbox, const char *resource,
		       struct carddav_db *davdb, unsigned flags)
{
    int ret = 0;
    VObjectIterator iter;
    struct carddav_data *cdata;
    char *version = NULL, *uid = NULL, *fullname = NULL;
    char *mimehdr;
    struct index_record *oldrecord = NULL, record;

    /* Validate the vCard data */
    if (!vcard || strcmp(vObjectName(vcard), "VCARD")) {
	txn->error.precond = CARDDAV_VALID_DATA;
	return HTTP_FORBIDDEN;
    }

    /* Fetch some important properties */
    initPropIterator(&iter, vcard);
    while (moreIteration(&iter)) {
	VObject *prop = nextVObject(&iter);
	const char *name = vObjectName(prop);

	if (!strcmp(name, "VERSION")) {
	    version = fakeCString(vObjectUStringZValue(prop));
	    if (strcmp(version, "3.0")) {
		txn->error.precond = CARDDAV_SUPP_DATA;
		goto done;
		ret = HTTP_FORBIDDEN;
	    }
	}
	else if (!strcmp(name, "UID")) {
	    uid = fakeCString(vObjectUStringZValue(prop));
	}
	else if (!strcmp(name, "FN")) {
	    fullname = fakeCString(vObjectUStringZValue(prop));
	}
    }

    /* Sanity check data */
    if (!version || !uid || !fullname) {
	txn->error.precond = CARDDAV_VALID_DATA;
	ret = HTTP_FORBIDDEN;
	goto done;
    }

    /* Check for existing vCard UID */
    carddav_lookup_uid(davdb, uid, 0, &cdata);
    if (!(flags & NO_DUP_CHECK) &&
	cdata->dav.mailbox && !strcmp(cdata->dav.mailbox, mailbox->name) &&
	strcmp(cdata->dav.resource, resource)) {
	/* CARDDAV:no-uid-conflict */
	char *owner = mboxname_to_userid(cdata->dav.mailbox);
	mboxname_hiersep_toexternal(&httpd_namespace, owner, 0);

	txn->error.precond = CARDDAV_UID_CONFLICT;
	assert(!buf_len(&txn->buf));
	buf_printf(&txn->buf, "%s/user/%s/%s/%s",
		   namespace_addressbook.prefix, owner,
		   strrchr(cdata->dav.mailbox, '.')+1, cdata->dav.resource);
	txn->error.resource = buf_cstring(&txn->buf);
	ret = HTTP_FORBIDDEN;
	goto done;
    }

    if (cdata->dav.imap_uid) {
	/* Fetch index record for the resource */
	oldrecord = &record;
	mailbox_find_index_record(mailbox, cdata->dav.imap_uid, oldrecord);
    }

    /* Create and cache RFC 5322 header fields for resource */
    mimehdr = charset_encode_mimeheader(fullname, 0);
    spool_cache_header(xstrdup("Subject"), mimehdr, txn->req_hdrs);

    /* XXX - validate uid for mime safety? */
    if (strchr(uid, '@')) {
	spool_cache_header(xstrdup("Message-ID"), xstrdup(uid), txn->req_hdrs);
    }
    else {
	assert(!buf_len(&txn->buf));
	buf_printf(&txn->buf, "<%s@%s>", uid, config_servername);
	spool_cache_header(xstrdup("Message-ID"),
			   buf_release(&txn->buf), txn->req_hdrs);
    }

    assert(!buf_len(&txn->buf));
    buf_printf(&txn->buf, "text/vcard; version=%s; charset=utf-8", version);
    spool_cache_header(xstrdup("Content-Type"),
		       buf_release(&txn->buf), txn->req_hdrs);

    buf_printf(&txn->buf, "attachment;\r\n\tfilename=\"%s\"", resource);
    spool_cache_header(xstrdup("Content-Disposition"),
		       buf_release(&txn->buf), txn->req_hdrs);

    /* Store the resource */
    ret = dav_store_resource(txn, buf_cstring(&txn->req_body.payload), 0,
			     mailbox, oldrecord, NULL, 0);

  done:
    if (uid) free(uid);
    if (version) free(version);
    if (fullname) free(fullname);

    return ret;
}


/* Callback to fetch DAV:getcontenttype */
static int propfind_getcontenttype(const xmlChar *name, xmlNsPtr ns,
				   struct propfind_ctx *fctx,
				   xmlNodePtr resp __attribute__((unused)),
				   struct propstat propstat[],
				   void *rock __attribute__((unused)))
{
    buf_setcstr(&fctx->buf, "text/vcard; charset=utf-8");

    xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
		 name, ns, BAD_CAST buf_cstring(&fctx->buf), 0);

    return 0;
}


/* Callback to fetch DAV:resourcetype */
static int propfind_restype(const xmlChar *name, xmlNsPtr ns,
			    struct propfind_ctx *fctx,
			    xmlNodePtr resp,
			    struct propstat propstat[],
			    void *rock __attribute__((unused)))
{
    xmlNodePtr node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV],
				   &propstat[PROPSTAT_OK], name, ns, NULL, 0);

    if (!fctx->record) {
	xmlNewChild(node, NULL, BAD_CAST "collection", NULL);

	if (fctx->req_tgt->collection) {
	    ensure_ns(fctx->ns, NS_CARDDAV, resp->parent,
		      XML_NS_CARDDAV, "C");
	    xmlNewChild(node, fctx->ns[NS_CARDDAV],
			BAD_CAST "addressbook", NULL);
	}
    }

    return 0;
}


/* Callback to prescreen/fetch CARDDAV:address-data */
static int propfind_addrdata(const xmlChar *name, xmlNsPtr ns,
			     struct propfind_ctx *fctx,
			     xmlNodePtr resp __attribute__((unused)),
			     struct propstat propstat[],
			     void *rock)
{
    xmlNodePtr prop = (xmlNodePtr) rock;
    const char *data = NULL;
    unsigned long datalen = 0;

    if (propstat) {
	if (!fctx->record) return HTTP_NOT_FOUND;

	if (!fctx->msg_base) {
	    mailbox_map_message(fctx->mailbox, fctx->record->uid,
				&fctx->msg_base, &fctx->msg_size);
	}
	if (!fctx->msg_base) return HTTP_SERVER_ERROR;

	data = fctx->msg_base + fctx->record->header_size;
	datalen = fctx->record->size - fctx->record->header_size;
    }

    return propfind_getdata(name, ns, fctx, propstat, prop, carddav_mime_types,
			    CARDDAV_SUPP_DATA, data, datalen);
}


/* Callback to fetch CARDDAV:addressbook-home-set */
int propfind_abookhome(const xmlChar *name, xmlNsPtr ns,
		       struct propfind_ctx *fctx,
		       xmlNodePtr resp __attribute__((unused)),
		       struct propstat propstat[],
		       void *rock)
{
    xmlNodePtr node;
    xmlNodePtr expand = (xmlNodePtr) rock;

    if (!(namespace_addressbook.enabled && fctx->req_tgt->user))
	return HTTP_NOT_FOUND;

    node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			name, ns, NULL, 0);

    buf_reset(&fctx->buf);
    buf_printf(&fctx->buf, "%s/user/%.*s/", namespace_addressbook.prefix,
	       (int) fctx->req_tgt->userlen, fctx->req_tgt->user);

    if (expand) {
	/* Return properties for this URL */
	expand_property(expand, fctx, buf_cstring(&fctx->buf),
			&carddav_parse_path, carddav_props, node, 0);

    }
    else {
	/* Return just the URL */
	xml_add_href(node, fctx->ns[NS_DAV], buf_cstring(&fctx->buf));
    }

    return 0;
}


/* Callback to fetch CARDDAV:supported-address-data */
static int propfind_suppaddrdata(const xmlChar *name, xmlNsPtr ns,
				 struct propfind_ctx *fctx,
				 xmlNodePtr resp __attribute__((unused)),
				 struct propstat propstat[],
				 void *rock __attribute__((unused)))
{
    xmlNodePtr node;
    struct mime_type_t *mime;

    if (!fctx->req_tgt->collection) return HTTP_NOT_FOUND;

    node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			name, ns, NULL, 0);

    for (mime = carddav_mime_types; mime->content_type; mime++) {
	xmlNodePtr type = xmlNewChild(node, fctx->ns[NS_CARDDAV],
				      BAD_CAST "address-data-type", NULL);

	/* Trim any charset from content-type */
	buf_reset(&fctx->buf);
	buf_printf(&fctx->buf, "%.*s",
		   (int) strcspn(mime->content_type, ";"), mime->content_type);

	xmlNewProp(type, BAD_CAST "content-type",
		   BAD_CAST buf_cstring(&fctx->buf));

	if (mime->version)
	    xmlNewProp(type, BAD_CAST "version", BAD_CAST mime->version);
    }

    buf_reset(&fctx->buf);

    return 0;
}


static int report_card_query(struct transaction_t *txn,
			     struct meth_params *rparams __attribute__((unused)),
			     xmlNodePtr inroot, struct propfind_ctx *fctx)
{
    int ret = 0;
    xmlNodePtr node;

    fctx->filter_crit = (void *) 0xDEADBEEF;  /* placeholder until we filter */
    fctx->open_db = (db_open_proc_t) &my_carddav_open;
    fctx->close_db = (db_close_proc_t) &my_carddav_close;
    fctx->lookup_resource = (db_lookup_proc_t) &carddav_lookup_resource;
    fctx->foreach_resource = (db_foreach_proc_t) &carddav_foreach;
    fctx->proc_by_resource = &propfind_by_resource;

    /* Parse children element of report */
    for (node = inroot->children; node; node = node->next) {
	if (node->type == XML_ELEMENT_NODE) {
	    if (!xmlStrcmp(node->name, BAD_CAST "filter")) {
		txn->error.precond = CARDDAV_SUPP_FILTER;
		return HTTP_FORBIDDEN;
	    }
	}
    }

    if (fctx->depth++ > 0) {
	/* Addressbook collection(s) */
	if (txn->req_tgt.collection) {
	    /* Add response for target addressbook collection */
	    propfind_by_collection(txn->req_tgt.mboxname, 0, 0, fctx);
	}
	else {
	    /* Add responses for all contained addressbook collections */
	    strlcat(txn->req_tgt.mboxname, ".%", sizeof(txn->req_tgt.mboxname));
	    mboxlist_findall(NULL,  /* internal namespace */
			     txn->req_tgt.mboxname, 1, httpd_userid, 
			     httpd_authstate, propfind_by_collection, fctx);
	}

	if (fctx->davdb) my_carddav_close(fctx->davdb);

	ret = *fctx->ret;
    }

    return (ret ? ret : HTTP_MULTI_STATUS);
}
