/* http_dav.c -- Routines for dealing with DAV properties in httpd
 *
 * Copyright (c) 1994-2011 Carnegie Mellon University.  All rights reserved.
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
 *
 *   - CALDAV:supported-calendar-component-set should be a bitmask in
 *     cyrus.index header Mailbox Options field
 *
 *   - CALDAV:schedule-calendar-transp should be a flag in
 *     cyrus.index header (Mailbox Options)
 *
 *   - DAV:creationdate sould be added to cyrus.header since it only
 *     gets set at creation time
 *
 *   - Should add a last_metadata_update field to cyrus.index header
 *     for use in PROPFIND, PROPPATCH, and possibly REPORT.
 *     This would get updated any time a mailbox annotation, mailbox
 *     acl, or quota root limit is changed
 *
 *   - Should we use cyrus.index header Format field to indicate
 *     CalDAV mailbox?
 *
 */


#include "http_dav.h"
#include "annotate.h"
#include "acl.h"
#include "caldav_db.h"
#include "global.h"
#include "http_err.h"
#include "http_proxy.h"
#include "imap_err.h"
#include "index.h"
#include "proxy.h"
#include "rfc822date.h"
#include "tok.h"
#include "xmalloc.h"
#include "xstrlcat.h"
#include "xstrlcpy.h"

#include <libxml/uri.h>


static int prin_parse_path(struct request_target_t *tgt, const char **errstr);

static struct propfind_params propfind_params = {
    &prin_parse_path, NULL, NULL, NULL
};

/* Namespace for WebDAV principals */
const struct namespace_t namespace_principal = {
    URL_NS_PRINCIPAL, "/principals", NULL, 1 /* auth */,
#ifdef WITH_CALDAV
    ALLOW_CAL |
#endif
#ifdef WITH_CARDDAV
    ALLOW_CARD |
#endif
    ALLOW_DAV,
    NULL, NULL, NULL, NULL,
    {
	{ NULL,			NULL },			/* ACL		*/
	{ NULL,			NULL },			/* COPY		*/
	{ NULL,			NULL },			/* DELETE	*/
	{ NULL,			NULL },			/* GET		*/
	{ NULL,			NULL },			/* HEAD		*/
	{ NULL,			NULL },			/* MKCALENDAR	*/
	{ NULL,			NULL },			/* MKCOL	*/
	{ NULL,			NULL },			/* MOVE		*/
	{ &meth_options,	NULL },			/* OPTIONS	*/
	{ NULL,			NULL },			/* POST		*/
	{ &meth_propfind,	&propfind_params },	/* PROPFIND	*/
	{ NULL,			NULL },			/* PROPPATCH	*/
	{ NULL,			NULL },			/* PUT		*/
	{ &meth_report,		NULL }			/* REPORT	*/
    }
};


/* Structure for property status */
struct propstat {
    xmlNodePtr root;
    long status;
    unsigned precond;
};

/* Index into propstat array */
enum {
    PROPSTAT_OK = 0,
    PROPSTAT_UNAUTH,
    PROPSTAT_FORBID,
    PROPSTAT_NOTFOUND,
    PROPSTAT_CONFLICT,
    PROPSTAT_FAILEDDEP,
    PROPSTAT_ERROR,
    PROPSTAT_OVERQUOTA
};
#define NUM_PROPSTAT 8

/* Linked-list of properties for fetching */
struct propfind_entry_list {
    xmlNodePtr prop;			/* Property */
    int (*get)(xmlNodePtr node,		/* Callback to fetch property */
	       struct propfind_ctx *fctx, xmlNodePtr resp,
	       struct propstat propstat[], void *rock);
    void *rock;				/* Add'l data to pass to callback */
    struct propfind_entry_list *next;
};


/* Context for patching (writing) properties */
struct proppatch_ctx {
    struct request_target_t *req_tgt;	/* parsed request target URL */
    unsigned meth;	    		/* requested Method */
    const char *mailboxname;		/* mailbox correspondng to collection */
    xmlNodePtr root;			/* root node to add to XML tree */
    xmlNsPtr *ns;			/* Array of our supported namespaces */
    struct txn *tid;			/* Transaction ID for annot writes */
    const char **errstr;		/* Error string to pass up to caller */
    int *ret;  				/* Return code to pass up to caller */
    struct buf buf;			/* Working buffer */
};


static const struct cal_comp_t {
    const char *name;
    unsigned long type;
} cal_comps[] = {
    { "VEVENT",    CAL_COMP_VEVENT },
    { "VTODO",     CAL_COMP_VTODO },
    { "VJOURNAL",  CAL_COMP_VJOURNAL },
    { "VFREEBUSY", CAL_COMP_VFREEBUSY },
    { "VTIMEZONE", CAL_COMP_VTIMEZONE },
    { "VALARM",	   CAL_COMP_VALARM },
    { NULL, 0 }
};

/* Bitmask of privilege flags */
enum {
    PRIV_IMPLICIT =		(1<<0),
    PRIV_INBOX =		(1<<1),
    PRIV_OUTBOX =		(1<<2)
};


/* Array of precondition/postcondition errors */
static const struct precond_t {
    const char *name;			/* Property name */
    unsigned ns;			/* Index into known namespace array */
} preconds[] = {
    /* Placeholder for zero (no) precondition code */
    { NULL, 0 },

    /* WebDAV (RFC 4918) preconditons */
    { "cannot-modify-protected-property", NS_DAV },

    /* WebDAV Versioning (RFC 3253) preconditions */
    { "supported-report", NS_DAV },
    { "resource-must-be-null", NS_DAV },

    /* WebDAV ACL (RFC 3744) preconditions */
    { "need-privileges", NS_DAV },
    { "no-invert", NS_DAV },
    { "no-abstract", NS_DAV },
    { "not-supported-privilege", NS_DAV },
    { "recognized-principal", NS_DAV },

    /* WebDAV Quota (RFC 4331) preconditions */
    { "quota-not-exceeded", NS_DAV },
    { "sufficient-disk-space", NS_DAV },

    /* WebDAV Extended MKCOL (RFC 5689) preconditions */
    { "valid-resourcetype", NS_DAV },

    /* WebDAV Sync (RFC 6578) preconditions */
    { "valid-sync-token", NS_DAV },
    { "number-of-matches-within-limits", NS_DAV },

    /* CalDAV (RFC 4791) preconditions */
    { "supported-calendar-data", NS_CALDAV },
    { "valid-calendar-data", NS_CALDAV },
    { "valid-calendar-object-resource", NS_CALDAV },
    { "supported-calendar-component", NS_CALDAV },
    { "calendar-collection-location-ok", NS_CALDAV },
    { "no-uid-conflict", NS_CALDAV },
    { "supported-filter", NS_CALDAV },
    { "valid-filter", NS_CALDAV },

    /* CalDAV Scheduling (RFC 6638) preconditions */
    { "valid-scheduling-message", NS_CALDAV },
    { "valid-organizer", NS_CALDAV },
    { "unique-scheduling-object-resource", NS_CALDAV },
    { "same-organizer-in-all-components", NS_CALDAV },
    { "allowed-organizer-scheduling-object-change", NS_CALDAV },
    { "allowed-attendee-scheduling-object-change", NS_CALDAV },

    /* iSchedule (draft-desruisseaux-ischedule) preconditions */
    { "verification-failed", NS_ISCHED }
};


/* Parse request-target path in /principals namespace */
static int prin_parse_path(struct request_target_t *tgt, const char **errstr)
{
    char *p = tgt->path;
    size_t len;

    if (!*p || !*++p) return 0;

    /* Skip namespace */
    len = strcspn(p, "/");
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
	if (!*p || !*++p) return 0;
    }
    else return HTTP_NOT_FOUND;  /* need to specify a userid */

    if (*p) {
	*errstr = "Too many segments in request target path";
	return HTTP_FORBIDDEN;
    }

    return 0;
}


unsigned get_preferences(struct transaction_t *txn)
{
    unsigned prefs = 0;
    const char **hdr;

    txn->flags.vary |= (VARY_BRIEF | VARY_PREFER);

    /* Check for Prefer header(s) */
    if ((hdr = spool_getheader(txn->req_hdrs, "Prefer"))) {
	int i;
	for (i = 0; hdr[i]; i++) {
	    tok_t tok;
	    char *token;

	    tok_init(&tok, hdr[i], ",\r\n", TOK_TRIMLEFT|TOK_TRIMRIGHT);
	    while ((token = tok_next(&tok))) {
		if (!strcmp(token, "return-minimal"))
		    prefs |= PREFER_MIN;
		else if (!strcmp(token, "return-representation"))
		    prefs |= PREFER_REP;
		else if (!strcmp(token, "depth-noroot"))
		    prefs |= PREFER_NOROOT;
	    }
	    tok_fini(&tok);
	}
    }

    /* Check for Brief header */
    if ((hdr = spool_getheader(txn->req_hdrs, "Brief")) &&
	!strcasecmp(hdr[0], "t")) {
	prefs |= PREFER_MIN;
    }

    return prefs;
}


static int add_privs(int rights, unsigned flags,
		     xmlNodePtr parent, xmlNodePtr root, xmlNsPtr *ns);


/* Ensure that we have a given namespace.  If it doesn't exist in what we
 * parsed in the request, create it and attach to 'node'.
 */
static int ensure_ns(xmlNsPtr *respNs, int ns, xmlNodePtr node,
		     const char *url, const char *prefix)
{
    if (!respNs[ns])
	respNs[ns] = xmlNewNs(node, BAD_CAST url, BAD_CAST prefix);

    /* XXX  check for errors */
    return 0;
}


/* Add namespaces declared in the request to our root node and Ns array */
static int xml_add_ns(xmlNodePtr req, xmlNsPtr *respNs, xmlNodePtr root)
{
    for (; req; req = req->next) {
	if (req->type == XML_ELEMENT_NODE) {
	    xmlNsPtr nsDef;

	    for (nsDef = req->nsDef; nsDef; nsDef = nsDef->next) {
		if (!xmlStrcmp(nsDef->href, BAD_CAST XML_NS_DAV))
		    ensure_ns(respNs, NS_DAV, root,
			      (const char *) nsDef->href,
			      (const char *) nsDef->prefix);
		else if (!xmlStrcmp(nsDef->href, BAD_CAST XML_NS_CALDAV))
		    ensure_ns(respNs, NS_CALDAV, root,
			      (const char *) nsDef->href,
			      (const char *) nsDef->prefix);
		else if (!xmlStrcmp(nsDef->href, BAD_CAST XML_NS_CS))
		    ensure_ns(respNs, NS_CS, root,
			      (const char *) nsDef->href,
			      (const char *) nsDef->prefix);
		else if (!xmlStrcmp(nsDef->href, BAD_CAST XML_NS_CYRUS))
		    ensure_ns(respNs, NS_CYRUS, root,
			      (const char *) nsDef->href,
			      (const char *) nsDef->prefix);
		else if (!xmlStrcmp(nsDef->href, BAD_CAST XML_NS_ICAL))
		    ensure_ns(respNs, NS_ICAL, root,
			      (const char *) nsDef->href,
			      (const char *) nsDef->prefix);
		else
		    xmlNewNs(root, nsDef->href, nsDef->prefix);
	    }
	}

	xml_add_ns(req->children, respNs, root);
    }

    /* XXX  check for errors */
    return 0;
}


/* Initialize an XML tree for a property response */
xmlNodePtr init_xml_response(const char *resp, int ns,
			     xmlNodePtr req, xmlNsPtr *respNs)
{
    /* Start construction of our XML response tree */
    xmlDocPtr doc = xmlNewDoc(BAD_CAST "1.0");
    xmlNodePtr root = NULL;

    if (!doc) return NULL;
    if (!(root = xmlNewNode(NULL, BAD_CAST resp))) return NULL;

    xmlDocSetRootElement(doc, root);

    /* Add namespaces from request to our response,
     * creating array of known namespaces that we can reference later.
     */
    memset(respNs, 0, NUM_NAMESPACE * sizeof(xmlNsPtr));
    xml_add_ns(req, respNs, root);

    /* Set namespace of root node */
    switch (ns) {
    case NS_ISCHED:
	ensure_ns(respNs, NS_ISCHED, root, XML_NS_ISCHED, NULL);
	break;

    case NS_CALDAV:
	ensure_ns(respNs, NS_CALDAV, root, XML_NS_CALDAV, "C");

    default:
	ensure_ns(respNs, NS_DAV, root, XML_NS_DAV, "D");
    }
    xmlSetNs(root, respNs[ns]);

    return root;
}

static xmlNodePtr xml_add_href(xmlNodePtr parent, xmlNsPtr ns,
			       const char *href)
{
    xmlChar *uri = xmlURIEscapeStr(BAD_CAST href, BAD_CAST "/");
    xmlNodePtr node = xmlNewChild(parent, ns, BAD_CAST "href", uri);

    free(uri);
    return node;
}

xmlNodePtr xml_add_error(xmlNodePtr root, struct error_t *err,
			 xmlNsPtr *avail_ns)
{
    xmlNsPtr ns[NUM_NAMESPACE];
    xmlNodePtr error, node;
    const struct precond_t *precond = &preconds[err->precond];
    unsigned err_ns = NS_DAV;
    const char *resp_desc = "responsedescription";

    if (precond->ns == NS_ISCHED) {
	err_ns = NS_ISCHED;
	resp_desc = "response-description";
    }

    if (!root) {
	error = root = init_xml_response("error", err_ns, NULL, ns);
	avail_ns = ns;
    }
    else error = xmlNewChild(root, NULL, BAD_CAST "error", NULL);

    if (precond->ns == NS_CALDAV) {
	ensure_ns(avail_ns, NS_CALDAV, root, XML_NS_CALDAV, "C");
    }
    node = xmlNewChild(error, avail_ns[precond->ns],
		       BAD_CAST precond->name, NULL);

    switch (err->precond) {
    case DAV_NEED_PRIVS:
	if (err->resource && err->rights) {
	    unsigned flags = 0;
	    size_t rlen = strlen(err->resource);
	    const char *p = err->resource + rlen;

	    node = xmlNewChild(node, NULL, BAD_CAST "resource", NULL);
	    xml_add_href(node, NULL, err->resource);

	    if (rlen > 6 && !strcmp(p-6, SCHED_INBOX))
		flags |= PRIV_INBOX;
	    else if (rlen > 7 && !strcmp(p-7, SCHED_OUTBOX))
		flags |= PRIV_OUTBOX;

	    add_privs(err->rights, flags, node, root, avail_ns);
	}
	break;

    case CALDAV_UNIQUE_OBJECT:
    case CALDAV_UID_CONFLICT:
	if (err->resource) xml_add_href(node, avail_ns[NS_DAV], err->resource);
	break;
    }

    if (err->desc) {
	xmlNewTextChild(error, NULL, BAD_CAST resp_desc, BAD_CAST err->desc);
    }

    return root;
}


/* Add a property 'name', of namespace 'ns', with content 'content',
 * and status code/string 'status' to propstat element 'stat'.
 * 'stat' will be created as necessary.
 */
static xmlNodePtr xml_add_prop(long status, xmlNsPtr davns,
			       struct propstat *propstat,
			       xmlNodePtr prop, xmlChar *content,
			       unsigned precond)
{
    xmlNodePtr newprop = NULL;

    if (!propstat->root) {
	propstat->root = xmlNewNode(davns, BAD_CAST "propstat");
	xmlNewChild(propstat->root, NULL, BAD_CAST "prop", NULL);
    }

    if (prop) newprop = xmlNewTextChild(propstat->root->children,
					prop->ns, prop->name, content);
    propstat->status = status;
    propstat->precond = precond;

    return newprop;
}


/* Add a response tree to 'root' for the specified href and 
   either error code or property list */
static int xml_add_response(struct propfind_ctx *fctx, long code)
{
    xmlNodePtr resp;

    resp = xmlNewChild(fctx->root, NULL, BAD_CAST "response", NULL);
    if (!resp) {
	*fctx->errstr = "Unable to add response XML element";
	*fctx->ret = HTTP_SERVER_ERROR;
	return HTTP_SERVER_ERROR;
    }
    xml_add_href(resp, NULL, fctx->req_tgt->path);

    if (code) {
	xmlNewChild(resp, NULL, BAD_CAST "status",
		    BAD_CAST http_statusline(code));
    }
    else {
	struct propstat propstat[NUM_PROPSTAT], *stat;
	struct propfind_entry_list *e;
	int i;

	memset(propstat, 0, NUM_PROPSTAT * sizeof(struct propstat));

	/* Process each property in the linked list */
	for (e = fctx->elist; e; e = e->next) {
	    if (e->get) {
		e->get(e->prop, fctx, resp, propstat, e->rock);
	    }
	    else if (!(fctx->prefer & PREFER_MIN)) {
		xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
			     &propstat[PROPSTAT_NOTFOUND], e->prop, NULL, 0);
	    }
	}

	/* Remove propstat 404 element if using return-minimal */
	stat = &propstat[PROPSTAT_NOTFOUND];
	if (stat->root && (fctx->prefer & PREFER_MIN)) {
	    xmlFreeNode(stat->root);
	    stat->root = NULL;
	}

	/* Check if we have any propstat elements */
	for (i = 0; i < NUM_PROPSTAT && !propstat[i].root; i++);
	if (i == NUM_PROPSTAT) {
	    /* Add an empty propstat 200 */
	    xml_add_prop(HTTP_OK, fctx->ns[NS_DAV],
			 &propstat[PROPSTAT_OK], NULL, NULL, 0);
	}

	/* Add status and optional error to the propstat elements
	   and then add them to response element */
	for (i = 0; i < NUM_PROPSTAT; i++) {
	    stat = &propstat[i];

	    if (stat->root) {
		xmlNewChild(stat->root, NULL, BAD_CAST "status",
			    BAD_CAST http_statusline(stat->status));
		if (stat->precond) {
		    struct error_t error = { NULL, stat->precond, NULL, 0 };
		    xml_add_error(stat->root, &error, fctx->ns);
		}

		xmlAddChild(resp, stat->root);
	    }
	}
    }

    fctx->record = NULL;

    return 0;
}


/* Callback to fetch DAV:add-member */
static int propfind_addmember(xmlNodePtr prop,
			      struct propfind_ctx *fctx,
			      xmlNodePtr resp __attribute__((unused)),
			      struct propstat propstat[],
			      void *rock __attribute__((unused)))
{
    if (fctx->req_tgt->collection) {
	xmlNodePtr node;
	size_t len;

	node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			    prop, NULL, 0);

	len = fctx->req_tgt->resource ?
	    (size_t) (fctx->req_tgt->resource - fctx->req_tgt->path) :
	    strlen(fctx->req_tgt->path);
	buf_reset(&fctx->buf);
	buf_printf(&fctx->buf, "%.*s", len, fctx->req_tgt->path);

	xml_add_href(node, NULL, buf_cstring(&fctx->buf));
    }
    else {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }

    return 0;
}


/* Callback to fetch DAV:getcontentlength */
static int propfind_getlength(xmlNodePtr prop,
			      struct propfind_ctx *fctx,
			      xmlNodePtr resp __attribute__((unused)),
			      struct propstat propstat[],
			      void *rock __attribute__((unused)))
{
    uint32_t len = 0;

    if (fctx->record) len = fctx->record->size - fctx->record->header_size;

    buf_reset(&fctx->buf);
    buf_printf(&fctx->buf, "%u", len);
    xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
		 prop, BAD_CAST buf_cstring(&fctx->buf), 0);

    return 0;
}


/* Callback to fetch DAV:getetag */
static int propfind_getetag(xmlNodePtr prop,
			    struct propfind_ctx *fctx,
			    xmlNodePtr resp __attribute__((unused)),
			    struct propstat propstat[],
			    void *rock __attribute__((unused)))
{
    if (fctx->record) {
	/* add DQUOTEs */
	buf_reset(&fctx->buf);
	buf_printf(&fctx->buf, "\"%s\"",
		   message_guid_encode(&fctx->record->guid));

	xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
		     prop, BAD_CAST buf_cstring(&fctx->buf), 0);
    }
    else {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }

    return 0;
}


/* Callback to fetch DAV:getlastmodified */
static int propfind_getlastmod(xmlNodePtr prop,
			       struct propfind_ctx *fctx,
			       xmlNodePtr resp __attribute__((unused)),
			       struct propstat propstat[],
			       void *rock __attribute__((unused)))
{
    if (fctx->record) {
	buf_ensure(&fctx->buf, 30);
	httpdate_gen(fctx->buf.s, fctx->buf.alloc,
		     fctx->record->internaldate);

	xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
		     prop, BAD_CAST fctx->buf.s, 0);
    }
    else {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }

    return 0;
}


/* Callback to fetch DAV:resourcetype */
static int propfind_restype(xmlNodePtr prop,
			    struct propfind_ctx *fctx,
			    xmlNodePtr resp,
			    struct propstat propstat[],
			    void *rock __attribute__((unused)))
{
    xmlNodePtr node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV],
				   &propstat[PROPSTAT_OK], prop, NULL, 0);

    if ((fctx->req_tgt->namespace != URL_NS_DEFAULT) && !fctx->record) {
	xmlNewChild(node, NULL, BAD_CAST "collection", NULL);

	switch (fctx->req_tgt->namespace) {
	case URL_NS_PRINCIPAL:
	    if (fctx->req_tgt->user)
		xmlNewChild(node, NULL, BAD_CAST "principal", NULL);
	    break;

	case URL_NS_CALENDAR:
	    if (fctx->req_tgt->collection) {
		ensure_ns(fctx->ns, NS_CALDAV, resp->parent, XML_NS_CALDAV, "C");
		if (!strcmp(fctx->req_tgt->collection, SCHED_INBOX)) {
		    xmlNewChild(node, fctx->ns[NS_CALDAV],
				BAD_CAST "schedule-inbox", NULL);
		}
		else if (!strcmp(fctx->req_tgt->collection, SCHED_OUTBOX)) {
		    xmlNewChild(node, fctx->ns[NS_CALDAV],
				BAD_CAST "schedule-outbox", NULL);
		}
		else {
		    xmlNewChild(node, fctx->ns[NS_CALDAV],
				BAD_CAST "calendar", NULL);
		}
	    }
	    break;
#if 0
	case URL_NS_ADDRESSBOOK:
	    if (fctx->req_tgt->collection) {
		ensure_ns(fctx->ns, NS_CARDDAV, resp->parent,
			  XML_NS_CARDDAV, "C");
		xmlNewChild(node, fctx->ns[NS_CARDDAV],
			    BAD_CAST "addressbook", NULL);
	    }
	    break;
#endif
	}
    }

    return 0;
}


/* Callback to "write" resourcetype property */
static int proppatch_restype(xmlNodePtr prop, unsigned set,
			     struct proppatch_ctx *pctx,
			     struct propstat propstat[],
			     void *rock __attribute__((unused)))
{
    unsigned precond = 0;

    if (set && (pctx->meth == METH_MKCOL || pctx->meth == METH_MKCALENDAR)) {
	/* "Writeable" for MKCOL/MKCALENDAR only */
	xmlNodePtr cur;

	for (cur = prop->children; cur; cur = cur->next) {
	    if (cur->type != XML_ELEMENT_NODE) continue;
	    /* Make sure we have valid resourcetypes for the collection */
	    if (xmlStrcmp(cur->name, BAD_CAST "collection") &&
		(xmlStrcmp(cur->name, BAD_CAST "calendar") ||
		 (pctx->req_tgt->namespace != URL_NS_CALENDAR))) break;
	}

	if (!cur) {
	    /* All resourcetypes are valid */
	    xml_add_prop(HTTP_OK, pctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			 prop, NULL, 0);

	    return 0;
	}

	/* Invalid resourcetype */
	precond = DAV_VALID_RESTYPE;
    }
    else {
	/* Protected property */
	precond = DAV_PROT_PROP;
    }

    xml_add_prop(HTTP_FORBIDDEN, pctx->ns[NS_DAV], &propstat[PROPSTAT_FORBID],
		 prop, NULL, precond);
	     
    *pctx->ret = HTTP_FORBIDDEN;

    return 0;
}


/* Callback to fetch DAV:sync-token and CS:getctag */
static int propfind_sync_token(xmlNodePtr prop,
			       struct propfind_ctx *fctx,
			       xmlNodePtr resp __attribute__((unused)),
			       struct propstat propstat[],
			       void *rock __attribute__((unused)))
{
    if (fctx->mailbox && !fctx->record) {
	buf_reset(&fctx->buf);
	buf_printf(&fctx->buf, XML_NS_CYRUS "sync/%u-" MODSEQ_FMT,
		   fctx->mailbox->i.uidvalidity,
		   fctx->mailbox->i.highestmodseq);

	xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
		     prop, BAD_CAST buf_cstring(&fctx->buf), 0);
    }
    else {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }

    return 0;
}


/* Callback to fetch DAV:supported-report-set */
static int propfind_reportset(xmlNodePtr prop,
			      struct propfind_ctx *fctx,
			      xmlNodePtr resp,
			      struct propstat propstat[],
			      void *rock __attribute__((unused)))
{
    xmlNodePtr s, r, top;

    top = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
		       prop, NULL, 0);

    if ((fctx->req_tgt->namespace == URL_NS_CALENDAR ||
	 fctx->req_tgt->namespace == URL_NS_ADDRESSBOOK) &&
	fctx->req_tgt->collection && !fctx->req_tgt->resource) {
	s = xmlNewChild(top, NULL, BAD_CAST "supported-report", NULL);
	r = xmlNewChild(s, NULL, BAD_CAST "report", NULL);
	ensure_ns(fctx->ns, NS_DAV, resp->parent, XML_NS_DAV, "D");
	xmlNewChild(r, fctx->ns[NS_DAV], BAD_CAST "sync-collection", NULL);
    }

    if (fctx->req_tgt->namespace == URL_NS_CALENDAR) {
	s = xmlNewChild(top, NULL, BAD_CAST "supported-report", NULL);
	r = xmlNewChild(s, NULL, BAD_CAST "report", NULL);
	ensure_ns(fctx->ns, NS_CALDAV, resp->parent, XML_NS_CALDAV, "C");
	xmlNewChild(r, fctx->ns[NS_CALDAV], BAD_CAST "calendar-query", NULL);

	s = xmlNewChild(top, NULL, BAD_CAST "supported-report", NULL);
	r = xmlNewChild(s, NULL, BAD_CAST "report", NULL);
	ensure_ns(fctx->ns, NS_CALDAV, resp->parent, XML_NS_CALDAV, "C");
	xmlNewChild(r, fctx->ns[NS_CALDAV], BAD_CAST "calendar-multiget", NULL);

	s = xmlNewChild(top, NULL, BAD_CAST "supported-report", NULL);
	r = xmlNewChild(s, NULL, BAD_CAST "report", NULL);
	ensure_ns(fctx->ns, NS_CALDAV, resp->parent, XML_NS_CALDAV, "C");
	xmlNewChild(r, fctx->ns[NS_CALDAV], BAD_CAST "free-busy-query", NULL);
    }

    return 0;
}


/* Callback to fetch DAV:principalurl */
static int propfind_principalurl(xmlNodePtr prop,
				 struct propfind_ctx *fctx,
				 xmlNodePtr resp __attribute__((unused)),
				 struct propstat propstat[],
				 void *rock __attribute__((unused)))
{
    xmlNodePtr node;

    if (fctx->req_tgt->namespace != URL_NS_PRINCIPAL) {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }
    else {
	node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			    prop, NULL, 0);

	buf_reset(&fctx->buf);
	if (fctx->req_tgt->user) {
	    buf_printf(&fctx->buf, "/principals/user/%.*s/",
		       fctx->req_tgt->userlen, fctx->req_tgt->user);
	}

	xml_add_href(node, NULL, buf_cstring(&fctx->buf));
    }

    return 0;
}


/* Callback to fetch DAV:owner */
static int propfind_owner(xmlNodePtr prop,
			  struct propfind_ctx *fctx,
			  xmlNodePtr resp __attribute__((unused)),
			  struct propstat propstat[],
			  void *rock __attribute__((unused)))
{
    xmlNodePtr node;

    node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			prop, NULL, 0);

    if ((fctx->req_tgt->namespace == URL_NS_CALENDAR) && fctx->req_tgt->user) {
	buf_reset(&fctx->buf);
	buf_printf(&fctx->buf, "/principals/user/%.*s/",
		   fctx->req_tgt->userlen, fctx->req_tgt->user);

	xml_add_href(node, NULL, buf_cstring(&fctx->buf));
    }

    return 0;
}


/* Add possibly 'abstract' supported-privilege 'priv_name', of namespace 'ns',
 * with description 'desc_str' to node 'root'.  For now, we alssume all
 * descriptions are English.
 */
static xmlNodePtr add_suppriv(xmlNodePtr root, const char *priv_name,
			      xmlNsPtr ns, int abstract, const char *desc_str)
{
    xmlNodePtr supp, priv, desc;

    supp = xmlNewChild(root, NULL, BAD_CAST "supported-privilege", NULL);
    priv = xmlNewChild(supp, NULL, BAD_CAST "privilege", NULL);
    xmlNewChild(priv, ns, BAD_CAST priv_name, NULL);
    if (abstract) xmlNewChild(supp, NULL, BAD_CAST "abstract", NULL);
    desc = xmlNewChild(supp, NULL, BAD_CAST "description", BAD_CAST desc_str);
    xmlNodeSetLang(desc, BAD_CAST "en");

    return supp;
}


/* Callback to fetch DAV:supported-privilege-set */
static int propfind_supprivset(xmlNodePtr prop,
			       struct propfind_ctx *fctx,
			       xmlNodePtr resp,
			       struct propstat propstat[],
			       void *rock __attribute__((unused)))
{
    xmlNodePtr set, all, agg, write;

    set = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
		       prop, NULL, 0);

    all = add_suppriv(set, "all", NULL, 0, "Any operation");

    agg = add_suppriv(all, "read", NULL, 0, "Read any object");
    add_suppriv(agg, "read-current-user-privilege-set", NULL, 1,
		"Read current user privilege set");

    ensure_ns(fctx->ns, NS_CALDAV, resp->parent, XML_NS_CALDAV, "C");
    add_suppriv(agg, "read-free-busy", fctx->ns[NS_CALDAV], 0,
		"Read free/busy time");

    write = add_suppriv(all, "write", NULL, 0, "Write any object");
    add_suppriv(write, "write-content", NULL, 0, "Write resource content");
    add_suppriv(write, "write-properties", NULL, 0, "Write properties");

    agg = add_suppriv(write, "bind", NULL, 0, "Add new member to collection");
    ensure_ns(fctx->ns, NS_CYRUS, resp->parent, XML_NS_CYRUS, "CY");
    add_suppriv(agg, "make-collection", fctx->ns[NS_CYRUS], 0,
		"Make new collection");
    add_suppriv(agg, "add-resource", fctx->ns[NS_CYRUS], 0,
		"Add new resource");

    agg = add_suppriv(write, "unbind", NULL, 0,
			 "Remove member from collection");
    add_suppriv(agg, "remove-collection", fctx->ns[NS_CYRUS], 0,
		"Remove collection");
    add_suppriv(agg, "remove-resource", fctx->ns[NS_CYRUS], 0,
		"Remove resource");

    agg = add_suppriv(all, "admin", fctx->ns[NS_CYRUS], 0,
			"Perform administrative operations");
    add_suppriv(agg, "read-acl", NULL, 1, "Read ACL");
    add_suppriv(agg, "write-acl", NULL, 1, "Write ACL");
    add_suppriv(agg, "unlock", NULL, 1, "Unlock resource");

    if (fctx->req_tgt->collection) {
	if (!strcmp(fctx->req_tgt->collection, SCHED_INBOX)) {
	    ensure_ns(fctx->ns, NS_CALDAV, resp->parent, XML_NS_CALDAV, "C");
	    agg = add_suppriv(all, "schedule-deliver", fctx->ns[NS_CALDAV], 0,
			      "Deliver scheduling messages");
	    add_suppriv(agg, "schedule-deliver-invite", fctx->ns[NS_CALDAV], 1,
			"Deliver scheduling messages from Organizers");
	    add_suppriv(agg, "schedule-deliver-reply", fctx->ns[NS_CALDAV], 1,
			"Deliver scheduling messages from Attendees");
	    add_suppriv(agg, "schedule-query-freebusy", fctx->ns[NS_CALDAV], 1,
			"Accept freebusy requests");
	}
	else if (!strcmp(fctx->req_tgt->collection, SCHED_OUTBOX)) {
	    ensure_ns(fctx->ns, NS_CALDAV, resp->parent, XML_NS_CALDAV, "C");
	    agg = add_suppriv(all, "schedule-send", fctx->ns[NS_CALDAV], 0,
			      "Send scheduling messages");
	    add_suppriv(agg, "schedule-send-invite", fctx->ns[NS_CALDAV], 1,
			"Send scheduling messages by Organizers");
	    add_suppriv(agg, "schedule-send-reply", fctx->ns[NS_CALDAV], 1,
			"Send scheduling messages by Attendees");
	    add_suppriv(agg, "schedule-send-freebusy", fctx->ns[NS_CALDAV], 1,
			"Submit freebusy requests");
	}
    }

    return 0;
}


/* Callback to fetch DAV:current-user-principal */
static int propfind_curprin(xmlNodePtr prop,
			    struct propfind_ctx *fctx,
			    xmlNodePtr resp __attribute__((unused)),
			    struct propstat propstat[],
			    void *rock __attribute__((unused)))
{
    xmlNodePtr node;

    node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			prop, NULL, 0);

    if (fctx->userid) {
	buf_reset(&fctx->buf);
	buf_printf(&fctx->buf, "/principals/user/%s/", fctx->userid);
	xml_add_href(node, NULL, buf_cstring(&fctx->buf));
    }
    else {
	xmlNewChild(node, NULL, BAD_CAST "unauthenticated", NULL);
    }

    return 0;
}


static int add_privs(int rights, unsigned flags,
		     xmlNodePtr parent, xmlNodePtr root, xmlNsPtr *ns)
{
    xmlNodePtr priv;

    if ((rights & DACL_ALL) == DACL_ALL) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	xmlNewChild(priv, NULL, BAD_CAST "all", NULL);
    }
    if ((rights & DACL_READ) == DACL_READ) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	xmlNewChild(priv, NULL, BAD_CAST "read", NULL);
	if (flags & PRIV_IMPLICIT) rights |= DACL_READFB;
    }
    if (rights & DACL_READFB) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	ensure_ns(ns, NS_CALDAV, root, XML_NS_CALDAV, "C");
	xmlNewChild(priv, ns[NS_CALDAV], BAD_CAST  "read-free-busy", NULL);
    }
    if ((rights & DACL_WRITE) == DACL_WRITE) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	xmlNewChild(priv, NULL, BAD_CAST "write", NULL);
    }
    if (rights & DACL_WRITECONT) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	xmlNewChild(priv, NULL, BAD_CAST "write-content", NULL);
    }
    if (rights & DACL_WRITEPROPS) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	xmlNewChild(priv, NULL, BAD_CAST "write-properties", NULL);
    }

    if (rights & (DACL_BIND|DACL_UNBIND|DACL_ADMIN)) {
	ensure_ns(ns, NS_CYRUS, root, XML_NS_CYRUS, "CY");
    }

    if ((rights & DACL_BIND) == DACL_BIND) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	xmlNewChild(priv, NULL, BAD_CAST "bind", NULL);
    }
    if (rights & DACL_MKCOL) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	xmlNewChild(priv, ns[NS_CYRUS], BAD_CAST "make-collection", NULL);
    }
    if (rights & DACL_ADDRSRC) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	xmlNewChild(priv, ns[NS_CYRUS], BAD_CAST "add-resource", NULL);
    }
    if ((rights & DACL_UNBIND) == DACL_UNBIND) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	xmlNewChild(priv, NULL, BAD_CAST "unbind", NULL);
    }
    if (rights & DACL_RMCOL) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	xmlNewChild(priv, ns[NS_CYRUS], BAD_CAST "remove-collection", NULL);
    }
    if (rights & DACL_RMRSRC) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	xmlNewChild(priv, ns[NS_CYRUS], BAD_CAST "remove-resource", NULL);
    }
    if (rights & DACL_ADMIN) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	xmlNewChild(priv, ns[NS_CYRUS], BAD_CAST  "admin", NULL);
    }

    if (rights & DACL_SCHED) {
	priv = xmlNewChild(parent, NULL, BAD_CAST "privilege", NULL);
	ensure_ns(ns, NS_CALDAV, root, XML_NS_CALDAV, "C");
	if (flags & PRIV_INBOX)
	    xmlNewChild(priv, ns[NS_CALDAV], BAD_CAST "schedule-deliver", NULL);
	else if (flags & PRIV_OUTBOX)
	    xmlNewChild(priv, ns[NS_CALDAV], BAD_CAST "schedule-send", NULL);
    }

    return 0;
}


/* Callback to fetch DAV:current-user-privilege-set */
static int propfind_curprivset(xmlNodePtr prop,
			       struct propfind_ctx *fctx,
			       xmlNodePtr resp,
			       struct propstat propstat[],
			       void *rock __attribute__((unused)))
{
    int rights;
    unsigned flags = PRIV_IMPLICIT;

    if (!fctx->mailbox) {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }
    else if (((rights =
	       cyrus_acl_myrights(fctx->authstate, fctx->mailbox->acl))
	      & DACL_READ) != DACL_READ) {
	xml_add_prop(HTTP_UNAUTHORIZED, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_UNAUTH], prop, NULL, 0);
    }
    else {
	xmlNodePtr set;

	/* Add in implicit rights */
	if (fctx->userisadmin) {
	    rights |= DACL_ADMIN;
	}
	else if (mboxname_userownsmailbox(fctx->userid, fctx->mailbox->name)) {
	    rights |= config_implicitrights;
	}

	/* Build the rest of the XML response */
	set = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			   prop, NULL, 0);

	if (fctx->req_tgt->collection) {
	    if (!strcmp(fctx->req_tgt->collection, SCHED_INBOX))
		flags |= PRIV_INBOX;
	    else if (!strcmp(fctx->req_tgt->collection, SCHED_OUTBOX))
		flags |= PRIV_OUTBOX;

	    add_privs(rights, flags, set, resp->parent, fctx->ns);
	}
    }

    return 0;
}


/* Callback to fetch DAV:acl */
static int propfind_acl(xmlNodePtr prop,
			struct propfind_ctx *fctx,
			xmlNodePtr resp,
			struct propstat propstat[],
			void *rock __attribute__((unused)))
{
    int rights;

    if (!fctx->mailbox) {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }
    else if (!((rights =
		cyrus_acl_myrights(fctx->authstate, fctx->mailbox->acl))
	       & DACL_ADMIN)) {
	xml_add_prop(HTTP_UNAUTHORIZED, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_UNAUTH], prop, NULL, 0);
    }
    else {
	xmlNodePtr acl;
	char *aclstr, *userid;
	unsigned flags = PRIV_IMPLICIT;

	if (fctx->req_tgt->collection) {
	    if (!strcmp(fctx->req_tgt->collection, SCHED_INBOX))
		flags |= PRIV_INBOX;
	    else if (!strcmp(fctx->req_tgt->collection, SCHED_OUTBOX))
		flags |= PRIV_OUTBOX;
	}

	/* Start the acl XML response */
	acl = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			   prop, NULL, 0);

	/* Parse the ACL string (userid/rights pairs) */
	userid = aclstr = xstrdup(fctx->mailbox->acl);

	while (userid) {
	    char *rightstr, *nextid;
	    xmlNodePtr ace, node;
	    int deny = 0;

	    rightstr = strchr(userid, '\t');
	    if (!rightstr) break;
	    *rightstr++ = '\0';
	
	    nextid = strchr(rightstr, '\t');
	    if (!nextid) break;
	    *nextid++ = '\0';

	    /* Check for negative rights */
	    /* XXX  Does this correspond to DAV:deny? */
	    if (*userid == '-') {
		deny = 1;
		userid++;
	    }

	    rights = cyrus_acl_strtomask(rightstr);

	    /* Add ace XML element for this userid/right pair */
	    ace = xmlNewChild(acl, NULL, BAD_CAST "ace", NULL);

	    /* XXX  Need to check for groups.
	     * Is there any IMAP equivalent to "unauthenticated"?
	     * Is there any DAV equivalent to "anonymous"?
	     */

	    node = xmlNewChild(ace, NULL, BAD_CAST "principal", NULL);
	    if (!strcmp(userid, fctx->userid))
		xmlNewChild(node, NULL, BAD_CAST "self", NULL);
	    else if ((strlen(userid) == fctx->req_tgt->userlen) &&
		     !strncmp(userid, fctx->req_tgt->user, fctx->req_tgt->userlen))
		xmlNewChild(node, NULL, BAD_CAST "owner", NULL);
	    else if (!strcmp(userid, "anyone"))
		xmlNewChild(node, NULL, BAD_CAST "authenticated", NULL);
	    else {
		buf_reset(&fctx->buf);
		buf_printf(&fctx->buf, "/principals/user/%s/", userid);
		xml_add_href(node, NULL, buf_cstring(&fctx->buf));
	    }

	    node = xmlNewChild(ace, NULL,
			       BAD_CAST (deny ? "deny" : "grant"), NULL);
	    add_privs(rights, flags, node, resp->parent, fctx->ns);

	    if (fctx->req_tgt->resource) {
		node = xmlNewChild(ace, NULL, BAD_CAST "inherited", NULL);
		buf_reset(&fctx->buf);
		buf_printf(&fctx->buf, "%.*s",
			   fctx->req_tgt->resource - fctx->req_tgt->path,
			   fctx->req_tgt->path);
		xml_add_href(node, NULL, buf_cstring(&fctx->buf));
	    }

	    userid = nextid;
	}

	if (aclstr) free(aclstr);
    }

    return 0;
}


/* Callback to fetch DAV:acl-restrictions */
static int propfind_aclrestrict(xmlNodePtr prop,
				struct propfind_ctx *fctx,
				xmlNodePtr resp __attribute__((unused)),
				struct propstat propstat[],
				void *rock __attribute__((unused)))
{
    xmlNodePtr node;

    node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			prop, NULL, 0);

    xmlNewChild(node, NULL, BAD_CAST "no-invert", NULL);

    return 0;
}


/* Callback to fetch DAV:principal-collection-set */
static int propfind_princolset(xmlNodePtr prop,
			       struct propfind_ctx *fctx,
			       xmlNodePtr resp __attribute__((unused)),
			       struct propstat propstat[],
			       void *rock __attribute__((unused)))
{
    xmlNodePtr node;

    node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			prop, NULL, 0);

    xmlNewChild(node, NULL, BAD_CAST "href", BAD_CAST "/principals/");

    return 0;
}


/* Callback to fetch DAV:quota-available-bytes and DAV:quota-used-bytes */
static int propfind_quota(xmlNodePtr prop,
			  struct propfind_ctx *fctx,
			  xmlNodePtr resp __attribute__((unused)),
			  struct propstat propstat[],
			  void *rock __attribute__((unused)))
{
    static char prevroot[MAX_MAILBOX_BUFFER];
    char foundroot[MAX_MAILBOX_BUFFER], *qr = NULL;

    if (fctx->mailbox) {
	/* Use the quotaroot as specified in mailbox header */
	qr = fctx->mailbox->quotaroot;
    }
    else {
	/* Find the quotaroot governing this hierarchy */
	if (quota_findroot(foundroot, sizeof(foundroot), fctx->req_tgt->mboxname)) {
	    qr = foundroot;
	}
    }

    if (qr) {
	if (!fctx->quota.root ||
	    strcmp(fctx->quota.root, qr)) {
	    /* Different quotaroot - read it */

	    syslog(LOG_DEBUG, "reading quota for '%s'", qr);

	    fctx->quota.root = strcpy(prevroot, qr);

	    quota_read(&fctx->quota, NULL, 0);
	}

	buf_reset(&fctx->buf);
	if (!xmlStrcmp(prop->name, BAD_CAST "quota-available-bytes")) {
	    /* Calculate limit in bytes and subtract usage */
	    uquota_t limit = fctx->quota.limit * QUOTA_UNITS;

	    buf_printf(&fctx->buf, UQUOTA_T_FMT, limit - fctx->quota.used);
	}
	else if (fctx->record) {
	    /* Bytes used by resource */
	    buf_printf(&fctx->buf, "%u", fctx->record->size);
	}
	else if (fctx->mailbox) {
	    /* Bytes used by calendar collection */
	    buf_printf(&fctx->buf, UQUOTA_T_FMT,
		       fctx->mailbox->i.quota_mailbox_used);
	}
	else {
	    /* Bytes used by entire hierarchy */
	    buf_printf(&fctx->buf, UQUOTA_T_FMT, fctx->quota.used);
	}

	xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
		     prop, BAD_CAST buf_cstring(&fctx->buf), 0);
    }
    else {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }

    return 0;
}


/* Callback to fetch CALDAV:calendar-data */
static int propfind_caldata(xmlNodePtr prop,
			    struct propfind_ctx *fctx,
			    xmlNodePtr resp,
			    struct propstat propstat[],
			    void *rock __attribute__((unused)))
{
    ensure_ns(fctx->ns, NS_CALDAV, resp->parent, XML_NS_CALDAV, "C");
    if (fctx->record) {
	xmlNodePtr data;

	if (!fctx->msg_base) {
	    mailbox_map_message(fctx->mailbox, fctx->record->uid,
				&fctx->msg_base, &fctx->msg_size);
	}

	data = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			    prop, NULL, 0);
	xmlAddChild(data,
		    xmlNewCDataBlock(fctx->root->doc,
				     BAD_CAST fctx->msg_base +
				     fctx->record->header_size,
				     fctx->record->size -
				     fctx->record->header_size));

	fctx->fetcheddata = 1;
    }
    else {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }

    return 0;
}


/* Callback to fetch CALDAV:calendar-home-set,
 * CALDAV:schedule-inbox-URL, CALDAV:schedule-outbox-URL,
 * and CALDAV:schedule-default-calendar-URL
 */
static int propfind_calurl(xmlNodePtr prop,
			   struct propfind_ctx *fctx,
			   xmlNodePtr resp,
			   struct propstat propstat[],
			   void *rock)
{
    xmlNodePtr node;
    const char *cal = (const char *) rock;

    ensure_ns(fctx->ns, NS_CALDAV, resp->parent, XML_NS_CALDAV, "C");
    if (fctx->userid &&
	/* sched-def-cal-URL only defined on sched-inbox-URL */
	((fctx->req_tgt->namespace == URL_NS_CALENDAR &&
	  fctx->req_tgt->collection && cal &&
	  !strcmp(fctx->req_tgt->collection, SCHED_INBOX) &&
	  !strcmp(cal, SCHED_DEFAULT))
	 /* others only defined on principals */
	 || (fctx->req_tgt->namespace == URL_NS_PRINCIPAL))) {
	node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			    prop, NULL, 0);

	buf_reset(&fctx->buf);
	buf_printf(&fctx->buf, "/calendars/user/%s/%s", fctx->userid,
		   cal ? cal : "");

	xml_add_href(node, fctx->ns[NS_DAV], buf_cstring(&fctx->buf));
    }
    else {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }

    return 0;
}


/* Callback to fetch CALDAV:supported-calendar-component-set */
static int propfind_calcompset(xmlNodePtr prop,
			       struct propfind_ctx *fctx,
			       xmlNodePtr resp __attribute__((unused)),
			       struct propstat propstat[],
			       void *rock __attribute__((unused)))
{
    struct annotation_data attrib;
    const char *value = NULL;
    unsigned long types;
    int r = 0;

    if ((fctx->req_tgt->namespace == URL_NS_CALENDAR) &&
	fctx->req_tgt->collection && !fctx->req_tgt->resource) {
	const char *prop_annot =
	    ANNOT_NS "CALDAV:supported-calendar-component-set";

	if (!(r = annotatemore_lookup(fctx->mailbox->name, prop_annot,
				      /* shared */ "", &attrib))
	    && attrib.value) {
	    value = attrib.value;
	}
    }

    if (r) {
	xml_add_prop(HTTP_SERVER_ERROR, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_ERROR], prop, NULL, 0);
    }
    else if (value && (types = strtoul(value, NULL, 10))) {
	xmlNodePtr set, node;
	const struct cal_comp_t *comp;

	set = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			   prop, NULL, 0);
	/* Create "comp" elements from the stored bitmask */
	for (comp = cal_comps; comp->name; comp++) {
	    if (types & comp->type) {
		node = xmlNewChild(set, fctx->ns[NS_CALDAV],
				   BAD_CAST "comp", NULL);
		xmlNewProp(node, BAD_CAST "name", BAD_CAST comp->name);
	    }
	}
    }
    else {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }

    return 0;
}


/* Callback to write supported-calendar-component-set property */
static int proppatch_calcompset(xmlNodePtr prop, unsigned set,
				struct proppatch_ctx *pctx,
				struct propstat propstat[],
				void *rock __attribute__((unused)))
{
    int r = 0;
    unsigned precond = 0;

    if ((pctx->req_tgt->namespace == URL_NS_CALENDAR) &&
	set && (pctx->meth == METH_MKCOL || pctx->meth == METH_MKCALENDAR)) {
	/* "Writeable" for MKCOL/MKCALENDAR only */
	xmlNodePtr cur;
	unsigned long types = 0;

	/* Work through the given list of components */
	for (cur = prop->children; cur; cur = cur->next) {
	    xmlChar *name;
	    const struct cal_comp_t *comp;

	    /* Make sure its a "comp" element with a "name" */
	    if (cur->type != XML_ELEMENT_NODE) continue;
	    if (xmlStrcmp(cur->name, BAD_CAST "comp") ||
		!(name = xmlGetProp(cur, BAD_CAST "name"))) break;

	    /* Make sure we have a valid component type */
	    for (comp = cal_comps;
		 comp->name && xmlStrcmp(name, BAD_CAST comp->name); comp++);
	    if (comp->name) types |= comp->type;   /* found match in our list */
	    else break;	    	     		   /* no match - invalid type */
	}

	if (!cur) {
	    /* All component types are valid */
	    const char *prop_annot =
		ANNOT_NS "CALDAV:supported-calendar-component-set";

	    buf_reset(&pctx->buf);
	    buf_printf(&pctx->buf, "%lu", types);
	    if (!(r = annotatemore_write_entry(pctx->mailboxname,
					       prop_annot, /* shared */ "",
					       buf_cstring(&pctx->buf), NULL,
					       buf_len(&pctx->buf), 0,
					       &pctx->tid))) {
		xml_add_prop(HTTP_OK, pctx->ns[NS_DAV],
			     &propstat[PROPSTAT_OK], prop, NULL, 0);
	    }
	    else {
		xml_add_prop(HTTP_SERVER_ERROR, pctx->ns[NS_DAV],
			     &propstat[PROPSTAT_ERROR], prop, NULL, 0);
	    }

	    return 0;
	}

	/* Invalid component type */
	precond = CALDAV_SUPP_COMP;
    }
    else {
	/* Protected property */
	precond = DAV_PROT_PROP;
    }

    xml_add_prop(HTTP_FORBIDDEN, pctx->ns[NS_DAV], &propstat[PROPSTAT_FORBID],
		 prop, NULL, precond);
	     
    *pctx->ret = HTTP_FORBIDDEN;

    return 0;
}

#ifdef WITH_CALDAV_SCHED
/* Callback to fetch CALDAV:schedule-tag */
static int propfind_schedtag(xmlNodePtr prop,
			     struct propfind_ctx *fctx,
			     xmlNodePtr resp,
			     struct propstat propstat[],
			     void *rock __attribute__((unused)))
{
    struct caldav_data *cdata = (struct caldav_data *) fctx->data;

    ensure_ns(fctx->ns, NS_CALDAV, resp->parent, XML_NS_CALDAV, "C");
    if (cdata->sched_tag) {
	/* add DQUOTEs */
	buf_reset(&fctx->buf);
	buf_printf(&fctx->buf, "\"%s\"", cdata->sched_tag);

	xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
		     prop, BAD_CAST buf_cstring(&fctx->buf), 0);
    }
    else {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }

    return 0;
}


/* Callback to fetch CALDAV:calendar-user-address-set */
static int propfind_caluseraddr(xmlNodePtr prop,
				struct propfind_ctx *fctx,
				xmlNodePtr resp,
				struct propstat propstat[],
				void *rock __attribute__((unused)))
{
    xmlNodePtr node;

    ensure_ns(fctx->ns, NS_CALDAV, resp->parent, XML_NS_CALDAV, "C");
    if (fctx->userid) {
	node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			    prop, NULL, 0);

	/* XXX  This needs to be done via an LDAP/DB lookup */
	buf_reset(&fctx->buf);
	buf_printf(&fctx->buf, "mailto:%s@%s", fctx->userid, config_servername);

	xmlNewChild(node, fctx->ns[NS_DAV], BAD_CAST "href",
		    BAD_CAST buf_cstring(&fctx->buf));
    }
    else {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }

    return 0;
}


/* Callback to fetch CALDAV:schedule-calendar-transp */
static int propfind_caltransp(xmlNodePtr prop,
			      struct propfind_ctx *fctx,
			      xmlNodePtr resp,
			      struct propstat propstat[],
			      void *rock __attribute__((unused)))
{
    struct annotation_data attrib;
    const char *value = NULL;
    int r = 0;

    if ((fctx->req_tgt->namespace == URL_NS_CALENDAR) &&
	fctx->req_tgt->collection && !fctx->req_tgt->resource) {
	const char *prop_annot =
	    ANNOT_NS "CALDAV:schedule-calendar-transp";

	if (!(r = annotatemore_lookup(fctx->mailbox->name, prop_annot,
				      /* shared */ "", &attrib))
	    && attrib.value) {
	    value = attrib.value;
	}
    }

    ensure_ns(fctx->ns, NS_CALDAV, resp->parent, XML_NS_CALDAV, "C");
    if (r) {
	xml_add_prop(HTTP_SERVER_ERROR, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_ERROR], prop, NULL, 0);
    }
    else if (value) {
	xmlNodePtr node;

	node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			    prop, NULL, 0);
	xmlNewChild(node, fctx->ns[NS_CALDAV], BAD_CAST value, NULL);
    }
    else {
	xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
		     &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }

    return 0;
}


/* Callback to write schedule-calendar-transp property */
static int proppatch_caltransp(xmlNodePtr prop, unsigned set,
			       struct proppatch_ctx *pctx,
			       struct propstat propstat[],
			       void *rock __attribute__((unused)))
{
    if ((pctx->req_tgt->namespace == URL_NS_CALENDAR) &&
	pctx->req_tgt->collection && !pctx->req_tgt->resource) {
	const char *prop_annot =
	    ANNOT_NS "CALDAV:schedule-calendar-transp";
	const char *transp = "";

	if (set) {
	    xmlNodePtr cur;

	    /* Find the value */
	    for (cur = prop->children; cur; cur = cur->next) {

		/* Make sure its a value we understand */
		if (cur->type != XML_ELEMENT_NODE) continue;
		if (!xmlStrcmp(cur->name, BAD_CAST "opaque") ||
		    !xmlStrcmp(cur->name, BAD_CAST "transparent")) {
		    transp = (const char *) cur->name;
		    break;
		}
		else {
		    /* Unknown value */
		    xml_add_prop(HTTP_CONFLICT, pctx->ns[NS_DAV],
				 &propstat[PROPSTAT_CONFLICT], prop, NULL, 0);

		    *pctx->ret = HTTP_FORBIDDEN;

		    return 0;
		}
	    }
	}

	if (!annotatemore_write_entry(pctx->mailboxname,
				      prop_annot, /* shared */ "",
				      transp, NULL,
				      strlen(transp), 0,
				      &pctx->tid)) {
	    xml_add_prop(HTTP_OK, pctx->ns[NS_DAV],
			 &propstat[PROPSTAT_OK], prop, NULL, 0);
	}
	else {
	    xml_add_prop(HTTP_SERVER_ERROR, pctx->ns[NS_DAV],
			 &propstat[PROPSTAT_ERROR], prop, NULL, 0);
	}
    }
    else {
	xml_add_prop(HTTP_FORBIDDEN, pctx->ns[NS_DAV],
		     &propstat[PROPSTAT_FORBID], prop, NULL, 0);

	*pctx->ret = HTTP_FORBIDDEN;
    }

    return 0;
}
#endif /* WITH_CALDAV_SCHED */

/* Callback to fetch properties from resource header */
static int propfind_fromhdr(xmlNodePtr prop,
			    struct propfind_ctx *fctx,
			    xmlNodePtr resp __attribute__((unused)),
			    struct propstat propstat[],
			    void *hdrname)
{
    if (fctx->record) {
	if (mailbox_cached_header((const char *) hdrname) != BIT32_MAX &&
	    !mailbox_cacherecord(fctx->mailbox, fctx->record)) {
	    unsigned size;
	    struct protstream *stream;
	    hdrcache_t hdrs = NULL; 
	    const char **hdr;

	    size = cacheitem_size(fctx->record, CACHE_HEADERS);
	    stream = prot_readmap(cacheitem_base(fctx->record,
						 CACHE_HEADERS), size);
	    hdrs = spool_new_hdrcache();
	    spool_fill_hdrcache(stream, NULL, hdrs, NULL);
	    prot_free(stream);

	    if ((hdr = spool_getheader(hdrs, (const char *) hdrname))) {
		xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			     prop, BAD_CAST hdr[0], 0);
	    }

	    spool_free_hdrcache(hdrs);

	    if (hdr) return 0;
	}
    }

    xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV], &propstat[PROPSTAT_NOTFOUND],
		 prop, NULL, 0);

    return 0;
}


/* Callback to read a property from annotation DB */
static int propfind_fromdb(xmlNodePtr prop,
			   struct propfind_ctx *fctx,
			   xmlNodePtr resp __attribute__((unused)),
			   struct propstat propstat[],
			   void *ns_prefix)
{
    struct annotation_data attrib;
    xmlNodePtr node;
    int r = 0;

    buf_reset(&fctx->buf);
    if (ns_prefix) {
	buf_printf(&fctx->buf, ANNOT_NS "%s:%s",
		   (const char *) ns_prefix, prop->name);
    }
    else {
	/* "dead" property - use hash of the namespace href as prefix */
	buf_printf(&fctx->buf, ANNOT_NS "%08X:%s",
		   strhash((const char *) prop->ns->href), prop->name);
    }

    memset(&attrib, 0, sizeof(struct annotation_data));

    if (fctx->mailbox && !fctx->record &&
	!(r = annotatemore_lookup(fctx->mailbox->name, buf_cstring(&fctx->buf),
				  /* shared */ "", &attrib))) {
	if (!attrib.value && 
	    !xmlStrcmp(prop->name, BAD_CAST "displayname")) {
	    /* Special case empty displayname -- use last segment of path */
	    attrib.value = strrchr(fctx->mailbox->name, '.') + 1;
	    attrib.size = strlen(attrib.value);
	}
    }

    if (r) {
	node = xml_add_prop(HTTP_SERVER_ERROR, fctx->ns[NS_DAV],
			    &propstat[PROPSTAT_ERROR], prop, NULL, 0);
    }
    else if (attrib.value) {
	node = xml_add_prop(HTTP_OK, fctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
			    prop, NULL, 0);
	xmlAddChild(node, xmlNewCDataBlock(fctx->root->doc,
					   BAD_CAST attrib.value, attrib.size));
    }
    else {
	node = xml_add_prop(HTTP_NOT_FOUND, fctx->ns[NS_DAV],
			    &propstat[PROPSTAT_NOTFOUND], prop, NULL, 0);
    }

    return 0;
}


/* Callback to write a property to annotation DB */
static int proppatch_todb(xmlNodePtr prop, unsigned set,
			  struct proppatch_ctx *pctx,
			  struct propstat propstat[], void *ns_prefix)
{
    xmlChar *freeme = NULL;
    const char *value = NULL;
    size_t len = 0;
    int r;

    buf_reset(&pctx->buf);
    if (ns_prefix) {
	buf_printf(&pctx->buf, ANNOT_NS "%s:%s",
		   (const char *) ns_prefix, BAD_CAST prop->name);
    }
    else {
	/* "dead" property - use hash of the namespace href as prefix */
	buf_printf(&pctx->buf, ANNOT_NS "%08X:%s",
		   strhash((const char *) prop->ns->href), BAD_CAST prop->name);
    }

    if (set) {
	freeme = xmlNodeGetContent(prop);
	value = (const char *) freeme;
	len = strlen(value);
    }

    if (!(r = annotatemore_write_entry(pctx->mailboxname,
				       buf_cstring(&pctx->buf), /* shared */ "",
				       value, NULL, len, 0,
				       &pctx->tid))) {
	xml_add_prop(HTTP_OK, pctx->ns[NS_DAV], &propstat[PROPSTAT_OK],
		     prop, NULL, 0);
    }
    else {
	xml_add_prop(HTTP_SERVER_ERROR, pctx->ns[NS_DAV],
		     &propstat[PROPSTAT_ERROR], prop, NULL, 0);
    }

    if (freeme) xmlFree(freeme);

    return 0;
}


/* Array of known "live" properties */
static const struct prop_entry {
    const char *name;			/* Property name */
    const char *ns;			/* Property namespace */
    unsigned allprop;			/* Should we fetch for allprop? */
    int (*get)(xmlNodePtr node,		/* Callback to fetch property */
	       struct propfind_ctx *fctx, xmlNodePtr resp,
	       struct propstat propstat[], void *rock);
    int (*put)(xmlNodePtr prop,		/* Callback to write property */
	       unsigned set, struct proppatch_ctx *pctx,
	       struct propstat propstat[], void *rock);
    void *rock;				/* Add'l data to pass to callback */
} prop_entries[] = {

    /* WebDAV (RFC 4918) properties */
    { "add-member", XML_NS_DAV, 0, propfind_addmember, NULL, NULL },
    { "creationdate", XML_NS_DAV, 1, NULL, NULL, NULL },
    { "displayname", XML_NS_DAV, 1, propfind_fromdb, proppatch_todb, "DAV" },
    { "getcontentlanguage", XML_NS_DAV, 1,
      propfind_fromhdr, NULL, "Content-Language" },
    { "getcontentlength", XML_NS_DAV, 1, propfind_getlength, NULL, NULL },
    { "getcontenttype", XML_NS_DAV, 1, propfind_fromhdr, NULL, "Content-Type" },
    { "getetag", XML_NS_DAV, 1, propfind_getetag, NULL, NULL },
    { "getlastmodified", XML_NS_DAV, 1, propfind_getlastmod, NULL, NULL },
    { "lockdiscovery", XML_NS_DAV, 1, NULL, NULL, NULL },
    { "resourcetype", XML_NS_DAV, 1,
      propfind_restype, proppatch_restype, NULL },
    { "supportedlock", XML_NS_DAV, 1, NULL, NULL, NULL },
    { "sync-token", XML_NS_DAV, 1, propfind_sync_token, NULL, NULL },

    /* WebDAV Versioning (RFC 3253) properties */
    { "supported-report-set", XML_NS_DAV, 0, propfind_reportset, NULL, NULL },

    /* WebDAV ACL (RFC 3744) properties */
    { "alternate-URI-set", XML_NS_DAV, 0, NULL, NULL, NULL },
    { "principal-URL", XML_NS_DAV, 0, propfind_principalurl, NULL, NULL },
    { "group-member-set", XML_NS_DAV, 0, NULL, NULL, NULL },
    { "group-membership", XML_NS_DAV, 0, NULL, NULL, NULL },
    { "owner", XML_NS_DAV, 0, propfind_owner, NULL, NULL },
    { "group", XML_NS_DAV, 0, NULL, NULL, NULL },
    { "supported-privilege-set", XML_NS_DAV, 0,
      propfind_supprivset, NULL, NULL },
    { "current-user-privilege-set", XML_NS_DAV, 0,
      propfind_curprivset, NULL, NULL },
    { "acl", XML_NS_DAV, 0, propfind_acl, NULL, NULL },
    { "acl-restrictions", XML_NS_DAV, 0, propfind_aclrestrict, NULL, NULL },
    { "inherited-acl-set", XML_NS_DAV, 0, NULL, NULL, NULL },
    { "principal-collection-set", XML_NS_DAV, 0,
      propfind_princolset, NULL, NULL },

    /* WebDAV Current Principal (RFC 5397) properties */
    { "current-user-principal", XML_NS_DAV, 0, propfind_curprin, NULL, NULL },

    /* WebDAV Quota (RFC 4331) properties */
    { "quota-available-bytes", XML_NS_DAV, 0, propfind_quota, NULL, NULL },
    { "quota-used-bytes", XML_NS_DAV, 0, propfind_quota, NULL, NULL },

    /* CalDAV (RFC 4791) properties */
    { "calendar-data", XML_NS_CALDAV, 0, propfind_caldata, NULL, NULL },
    { "calendar-description", XML_NS_CALDAV, 0,
      propfind_fromdb, proppatch_todb, "CALDAV" },
    { "calendar-home-set", XML_NS_CALDAV, 0, propfind_calurl, NULL, NULL },
    { "calendar-timezone", XML_NS_CALDAV, 0,
      propfind_fromdb, proppatch_todb, "CALDAV" },
    { "supported-calendar-component-set", XML_NS_CALDAV, 0,
      propfind_calcompset, proppatch_calcompset, NULL },
    { "supported-calendar-data", XML_NS_CALDAV, 0, NULL, NULL, NULL },
    { "max-resource-size", XML_NS_CALDAV, 0, NULL, NULL, NULL },
    { "min-date-time", XML_NS_CALDAV, 0, NULL, NULL, NULL },
    { "max-date-time", XML_NS_CALDAV, 0, NULL, NULL, NULL },
    { "max-instances", XML_NS_CALDAV, 0, NULL, NULL, NULL },
    { "max-attendees-per-instance", XML_NS_CALDAV, 0, NULL, NULL, NULL },

#ifdef WITH_CALDAV_SCHED
    /* CalDAV Scheduling (RFC 6638) properties */
    { "schedule-tag", XML_NS_CALDAV, 0, propfind_schedtag, NULL, NULL },
    { "schedule-inbox-URL", XML_NS_CALDAV, 0,
      propfind_calurl, NULL, SCHED_INBOX },
    { "schedule-outbox-URL", XML_NS_CALDAV, 0,
      propfind_calurl, NULL, SCHED_OUTBOX },
    { "schedule-default-calendar-URL", XML_NS_CALDAV, 0,
      propfind_calurl, NULL, SCHED_DEFAULT },
    { "schedule-calendar-transp", XML_NS_CALDAV, 0,
      propfind_caltransp, proppatch_caltransp, NULL },
    { "calendar-user-address-set", XML_NS_CALDAV, 0,
      propfind_caluseraddr, NULL, NULL },
    { "calendar-user-type", XML_NS_CALDAV, 0, NULL, NULL, NULL },
#endif /* WITH_CALDAV_SCHED */

    /* Calendar Server properties */
    { "getctag", XML_NS_CS, 1, propfind_sync_token, NULL, NULL },

    /* Apple iCal properties */
    { "calendar-color", XML_NS_ICAL, 0,
      propfind_fromdb, proppatch_todb, "iCAL" },
    { "calendar-order", XML_NS_ICAL, 0,
      propfind_fromdb, proppatch_todb, "iCAL" },

    { NULL, NULL, 0, NULL, NULL, NULL }
};


/* Parse the requested properties and create a linked list of fetch callbacks.
 * The list gets reused for each href if Depth > 0
 */
static int preload_proplist(xmlNodePtr proplist, struct propfind_ctx *fctx)
{
    xmlNodePtr prop;
    const struct prop_entry *entry;

    /* Iterate through requested properties */
    for (prop = proplist; prop; prop = prop->next) {
	if (prop->type == XML_ELEMENT_NODE) {
	    struct propfind_entry_list *nentry =
		xzmalloc(sizeof(struct propfind_entry_list));

	    /* Look for a match against our known properties */
	    for (entry = prop_entries;
		 entry->name && 
		     (strcmp((const char *) prop->name, entry->name) ||
		      strcmp((const char *) prop->ns->href, entry->ns));
		 entry++);

	    nentry->prop = prop;
	    if (entry->name) {
		/* Found a match */
		nentry->get = entry->get;
		nentry->rock = entry->rock;
	    }
	    else {
		/* No match, treat as a dead property */
		nentry->get = propfind_fromdb;
		nentry->rock = NULL;
	    }
	    nentry->next = fctx->elist;
	    fctx->elist = nentry;
	}
    }

    return 0;
}


/* Execute the given property patch instructions */
static int do_proppatch(struct proppatch_ctx *pctx, xmlNodePtr instr)
{
    struct propstat propstat[NUM_PROPSTAT];
    int i;

    memset(propstat, 0, NUM_PROPSTAT * sizeof(struct propstat));

    /* Iterate through propertyupdate children */
    for (; instr; instr = instr->next) {
	if (instr->type == XML_ELEMENT_NODE) {
	    xmlNodePtr prop;
	    unsigned set = 0;

	    if (!xmlStrcmp(instr->name, BAD_CAST "set")) set = 1;
	    else if ((pctx->meth == METH_PROPPATCH) &&
		     !xmlStrcmp(instr->name, BAD_CAST "remove")) set = 0;
	    else {
		syslog(LOG_INFO, "Unknown PROPPATCH instruction");
		*pctx->errstr = "Unknown PROPPATCH instruction";
		return HTTP_BAD_REQUEST;
	    }

	    /* Find child element */
	    for (prop = instr->children;
		 prop && prop->type != XML_ELEMENT_NODE; prop = prop->next);
	    if (!prop || xmlStrcmp(prop->name, BAD_CAST "prop")) {
		*pctx->errstr = "Missing prop element";
		return HTTP_BAD_REQUEST;
	    }

	    /* Iterate through requested properties */
	    for (prop = prop->children; prop; prop = prop->next) {
		if (prop->type == XML_ELEMENT_NODE) {
		    const struct prop_entry *entry;

		    /* Look for a match against our known properties */
		    for (entry = prop_entries;
			 entry->name &&
			     (strcmp((const char *) prop->name, entry->name) ||
			      strcmp((const char *) prop->ns->href, entry->ns));
			 entry++);

		    if (entry->name) {
			if (!entry->put) {
			    /* Protected property */
			    xml_add_prop(HTTP_FORBIDDEN, pctx->ns[NS_DAV],
					 &propstat[PROPSTAT_FORBID],
					 prop, NULL,
					 DAV_PROT_PROP);
			    *pctx->ret = HTTP_FORBIDDEN;
			}
			else {
			    /* Write "live" property */
			    entry->put(prop, set, pctx, propstat, entry->rock);
			}
		    }
		    else {
			/* Write "dead" property */
			proppatch_todb(prop, set, pctx, propstat, NULL);
		    }
		}
	    }
	}
    }

    /* One or more of the properties failed */
    if (*pctx->ret && propstat[PROPSTAT_OK].root) {
	/* 200 status must become 424 */
	propstat[PROPSTAT_FAILEDDEP].root = propstat[PROPSTAT_OK].root;
	propstat[PROPSTAT_FAILEDDEP].status = HTTP_FAILED_DEP;
	propstat[PROPSTAT_OK].root = NULL;
    }

    /* Add status and optional error to the propstat elements
       and then add them to the response element */
    for (i = 0; i < NUM_PROPSTAT; i++) {
	struct propstat *stat = &propstat[i];

	if (stat->root) {
	    xmlNewChild(stat->root, NULL, BAD_CAST "status",
			BAD_CAST http_statusline(stat->status));
	    if (stat->precond) {
		struct error_t error = { NULL, stat->precond, NULL, 0 };
		xml_add_error(stat->root, &error, pctx->ns);
	    }

	    xmlAddChild(pctx->root, stat->root);
	}
    }

    return 0;
}


/* Parse an XML body into a tree */
int parse_xml_body(struct transaction_t *txn, xmlNodePtr *root)
{
    const char **hdr;
    xmlParserCtxtPtr ctxt;
    xmlDocPtr doc = NULL;
    int r = 0;

    *root = NULL;

    /* Read body */
    if (!txn->flags.havebody) {
	txn->flags.havebody = 1;
	r = read_body(httpd_in, txn->req_hdrs, &txn->req_body, 1,
		      &txn->error.desc);
	if (r) {
	    txn->flags.close = 1;
	    return r;
	}
    }

    if (!buf_len(&txn->req_body)) return 0;

    /* Check Content-Type */
    if (!(hdr = spool_getheader(txn->req_hdrs, "Content-Type")) ||
	(!is_mediatype(hdr[0], "text/xml") &&
	 !is_mediatype(hdr[0], "application/xml"))) {
	txn->error.desc = "This method requires an XML body\r\n";
	return HTTP_BAD_MEDIATYPE;
    }

    /* Parse the XML request */
    ctxt = xmlNewParserCtxt();
    if (ctxt) {
	doc = xmlCtxtReadMemory(ctxt, buf_cstring(&txn->req_body),
				buf_len(&txn->req_body), NULL, NULL,
				XML_PARSE_NOWARNING);
	xmlFreeParserCtxt(ctxt);
    }
    if (!doc) {
	txn->error.desc = "Unable to parse XML body\r\n";
	return HTTP_BAD_REQUEST;
    }

    /* Get the root element of the XML request */
    if (!(*root = xmlDocGetRootElement(doc))) {
	txn->error.desc = "Missing root element in request\r\n";
	return HTTP_BAD_REQUEST;
    }

    return 0;
}


/* Perform an ACL request
 *
 * preconditions:
 *   DAV:no-ace-conflict
 *   DAV:no-protected-ace-conflict
 *   DAV:no-inherited-ace-conflict
 *   DAV:limited-number-of-aces
 *   DAV:deny-before-grant
 *   DAV:grant-only
 *   DAV:no-invert
 *   DAV:no-abstract
 *   DAV:not-supported-privilege
 *   DAV:missing-required-principal
 *   DAV:recognized-principal
 *   DAV:allowed-principal
 */
int meth_acl(struct transaction_t *txn, void *params)
{
    int ret = 0, r, rights;
    xmlDocPtr indoc = NULL;
    xmlNodePtr root, ace;
    char *server, *aclstr;
    struct mailbox *mailbox = NULL;
    struct buf acl = BUF_INITIALIZER;
    struct acl_params *aparams = (struct acl_params *) params;

    /* Response should not be cached */
    txn->flags.cc |= CC_NOCACHE;

    /* Make sure its a DAV resource */
    if (!(txn->req_tgt.allow & ALLOW_WRITE)) return HTTP_NOT_ALLOWED;

    /* Parse the path */
    if ((r = aparams->parse_path(&txn->req_tgt, &txn->error.desc))) return r;

    /* Make sure its a calendar collection */
    if (!txn->req_tgt.collection || txn->req_tgt.resource) {
	txn->error.desc = "ACLs can only be set on calendar collections\r\n";
	syslog(LOG_DEBUG, "Tried to set ACL on non-calendar collection");
	return HTTP_NOT_ALLOWED;
    }

    /* Locate the mailbox */
    if ((r = http_mlookup(txn->req_tgt.mboxname, &server, &aclstr, NULL))) {
	syslog(LOG_ERR, "mlookup(%s) failed: %s",
	       txn->req_tgt.mboxname, error_message(r));
	txn->error.desc = error_message(r);

	switch (r) {
	case IMAP_PERMISSION_DENIED: return HTTP_FORBIDDEN;
	case IMAP_MAILBOX_NONEXISTENT: return HTTP_NOT_FOUND;
	default: return HTTP_SERVER_ERROR;
	}
    }

    /* Check ACL for current user */
    rights =  aclstr ? cyrus_acl_myrights(httpd_authstate, aclstr) : 0;
    if (!(rights & DACL_ADMIN)) {
	/* DAV:need-privileges */
	txn->error.precond = DAV_NEED_PRIVS;
	txn->error.resource = txn->req_tgt.path;
	txn->error.rights = DACL_ADMIN;
	return HTTP_FORBIDDEN;
    }

    if (server) {
	/* Remote mailbox */
	struct backend *be;

	be = proxy_findserver(server, &http_protocol, httpd_userid,
			      &backend_cached, NULL, NULL, httpd_in);
	if (!be) return HTTP_UNAVAILABLE;

	return http_pipe_req_resp(be, txn);
    }

    /* Local Mailbox */

    /* Open mailbox for writing */
    if ((r = http_mailbox_open(txn->req_tgt.mboxname, &mailbox, LOCK_EXCLUSIVE))) {
	syslog(LOG_ERR, "http_mailbox_open(%s) failed: %s",
	       txn->req_tgt.mboxname, error_message(r));
	txn->error.desc = error_message(r);
	ret = HTTP_SERVER_ERROR;
	goto done;
    }

    /* Parse the ACL body */
    ret = parse_xml_body(txn, &root);
    if (!root) {
	txn->error.desc = "Missing request body\r\n";
	ret = HTTP_BAD_REQUEST;
    }
    if (ret) goto done;

    indoc = root->doc;

    /* Make sure its an DAV:acl element */
    if (xmlStrcmp(root->name, BAD_CAST "acl")) {
	txn->error.desc = "Missing acl element in ACL request\r\n";
	ret = HTTP_BAD_REQUEST;
	goto done;
    }

    /* Parse the DAV:ace elements */
    for (ace = root->children; ace; ace = ace->next) {
	if (ace->type == XML_ELEMENT_NODE) {
	    xmlNodePtr child = NULL, prin = NULL, privs = NULL;
	    const char *userid = NULL;
	    int deny = 0, rights = 0;
	    char rightstr[100];

	    for (child = ace->children; child; child = child->next) {
		if (child->type == XML_ELEMENT_NODE) {
		    if (!xmlStrcmp(child->name, BAD_CAST "principal")) {
			if (prin) {
			    txn->error.desc = "Multiple principals in ACE\r\n";
			    ret = HTTP_BAD_REQUEST;
			    goto done;
			}

			for (prin = child->children;
			     prin->type != XML_ELEMENT_NODE; prin = prin->next);
		    }
		    else if (!xmlStrcmp(child->name, BAD_CAST "grant")) {
			if (privs) {
			    txn->error.desc = "Multiple grant|deny in ACE\r\n";
			    ret = HTTP_BAD_REQUEST;
			    goto done;
			}

			for (privs = child->children;
			     privs->type != XML_ELEMENT_NODE; privs = privs->next);
		    }
		    else if (!xmlStrcmp(child->name, BAD_CAST "deny")) {
			if (privs) {
			    txn->error.desc = "Multiple grant|deny in ACE\r\n";
			    ret = HTTP_BAD_REQUEST;
			    goto done;
			}

			for (privs = child->children;
			     privs->type != XML_ELEMENT_NODE; privs = privs->next);
			deny = 1;
		    }
		    else if (!xmlStrcmp(child->name, BAD_CAST "invert")) {
			/* DAV:no-invert */
			txn->error.precond = DAV_NO_INVERT;
			ret = HTTP_FORBIDDEN;
			goto done;
		    }
		    else {
			txn->error.desc = "Unknown element in ACE\r\n";
			ret = HTTP_BAD_REQUEST;
			goto done;
		    }
		}
	    }

	    if (!xmlStrcmp(prin->name, BAD_CAST "self")) {
		userid = httpd_userid;
	    }
#if 0  /* XXX  Do we need to support this? */
	    else if (!xmlStrcmp(prin->name, BAD_CAST "owner")) {
		/* XXX construct userid from mailbox name */
	    }
#endif
	    else if (!xmlStrcmp(prin->name, BAD_CAST "authenticated")) {
		userid = "anyone";
	    }
	    else if (!xmlStrcmp(prin->name, BAD_CAST "href")) {
		xmlChar *href = xmlNodeGetContent(prin);
		struct request_target_t uri;
		const char *errstr = NULL;

		r = parse_uri(METH_UNKNOWN, (const char *) href, &uri, &errstr);
		if (!r &&
		    !strncmp("/principals/", uri.path, strlen("/principals/"))) {
		    uri.namespace = URL_NS_PRINCIPAL;
		    r = aparams->parse_path(&uri, &errstr);
		    if (!r && uri.user) userid = uri.user;
		}
		xmlFree(href);
	    }

	    if (!userid) {
		/* DAV:recognized-principal */
		txn->error.precond = DAV_RECOG_PRINC;
		ret = HTTP_FORBIDDEN;
		goto done;
	    }

	    for (; privs; privs = privs->next) {
		if (privs->type == XML_ELEMENT_NODE) {
		    xmlNodePtr priv = privs->children;
		    for (; priv->type != XML_ELEMENT_NODE; priv = priv->next);

		    if (aparams->acl_ext &&
			aparams->acl_ext(txn, priv, &rights)) {
			/* Extension (CalDAV) privileges */
			if (txn->error.precond) {
			    ret = HTTP_FORBIDDEN;
			    goto done;
			}
		    }
		    else if (!xmlStrcmp(priv->ns->href,
					BAD_CAST XML_NS_DAV)) {
			/* WebDAV privileges */
			if (!xmlStrcmp(priv->name,
				       BAD_CAST "all"))
			    rights |= DACL_ALL;
			else if (!xmlStrcmp(priv->name,
					    BAD_CAST "read"))
			    rights |= DACL_READ;
			else if (!xmlStrcmp(priv->name,
					    BAD_CAST "write"))
			    rights |= DACL_WRITE;
			else if (!xmlStrcmp(priv->name,
					    BAD_CAST "write-content"))
			    rights |= DACL_WRITECONT;
			else if (!xmlStrcmp(priv->name,
					    BAD_CAST "write-properties"))
			    rights |= DACL_WRITEPROPS;
			else if (!xmlStrcmp(priv->name,
					    BAD_CAST "bind"))
			    rights |= DACL_BIND;
			else if (!xmlStrcmp(priv->name,
					    BAD_CAST "unbind"))
			    rights |= DACL_UNBIND;
			else if (!xmlStrcmp(priv->name,
					    BAD_CAST "read-current-user-privilege-set")
				 || !xmlStrcmp(priv->name,
					       BAD_CAST "read-acl")
				 || !xmlStrcmp(priv->name,
					       BAD_CAST "write-acl")
				 || !xmlStrcmp(priv->name,
					       BAD_CAST "unlock")) {
			    /* DAV:no-abstract */
			    txn->error.precond = DAV_NO_ABSTRACT;
			    ret = HTTP_FORBIDDEN;
			    goto done;
			}
			else {
			    /* DAV:not-supported-privilege */
			    txn->error.precond = DAV_SUPP_PRIV;
			    ret = HTTP_FORBIDDEN;
			    goto done;
			}
		    }
		    else if (!xmlStrcmp(priv->ns->href,
				   BAD_CAST XML_NS_CYRUS)) {
			/* Cyrus-specific privileges */
			if (!xmlStrcmp(priv->name,
				       BAD_CAST "make-collection"))
			    rights |= DACL_MKCOL;
			else if (!xmlStrcmp(priv->name,
				       BAD_CAST "remove-collection"))
			    rights |= DACL_RMCOL;
			else if (!xmlStrcmp(priv->name,
				       BAD_CAST "add-resource"))
			    rights |= DACL_ADDRSRC;
			else if (!xmlStrcmp(priv->name,
				       BAD_CAST "remove-resource"))
			    rights |= DACL_RMRSRC;
			else if (!xmlStrcmp(priv->name,
				       BAD_CAST "admin"))
			    rights |= DACL_ADMIN;
			else {
			    /* DAV:not-supported-privilege */
			    txn->error.precond = DAV_SUPP_PRIV;
			    ret = HTTP_FORBIDDEN;
			    goto done;
			}
		    }
		    else {
			/* DAV:not-supported-privilege */
			txn->error.precond = DAV_SUPP_PRIV;
			ret = HTTP_FORBIDDEN;
			goto done;
		    }
		}
	    }

	    cyrus_acl_masktostr(rights, rightstr);
	    buf_printf(&acl, "%s%s\t%s\t",
		       deny ? "-" : "", userid, rightstr);
	}
    }

    if ((r = mboxlist_sync_setacls(txn->req_tgt.mboxname, buf_cstring(&acl)))) {
	syslog(LOG_ERR, "mboxlist_sync_setacls(%s) failed: %s",
	       txn->req_tgt.mboxname, error_message(r));
	txn->error.desc = error_message(r);
	ret = HTTP_SERVER_ERROR;
	goto done;
    }
    mailbox_set_acl(mailbox, buf_cstring(&acl), 0);

    response_header(HTTP_OK, txn);

  done:
    buf_free(&acl);
    if (indoc) xmlFreeDoc(indoc);
    if (mailbox) mailbox_unlock_index(mailbox, NULL);

    return ret;
}


/* Perform a GET/HEAD request */
int meth_get_dav(struct transaction_t *txn, void *params)
{
    int ret = 0, r, precond, rights;
    const char *msg_base = NULL, *data = NULL;
    unsigned long msg_size = 0, datalen, offset;
    struct resp_body_t *resp_body = &txn->resp_body;
    char *server, *acl;
    struct mailbox *mailbox = NULL;
    struct dav_data *ddata;
    struct index_record record;
    const char *etag = NULL;
    time_t lastmod = 0;
    struct get_params *gparams = (struct get_params *) params;

    /* Parse the path */
    if ((r = gparams->parse_path(&txn->req_tgt, &txn->error.desc))) return r;

    /* We don't handle GET on a calendar collection (yet) */
    if (!txn->req_tgt.resource) return HTTP_NO_CONTENT;

    /* Locate the mailbox */
    if ((r = http_mlookup(txn->req_tgt.mboxname, &server, &acl, NULL))) {
	syslog(LOG_ERR, "mlookup(%s) failed: %s",
	       txn->req_tgt.mboxname, error_message(r));
	txn->error.desc = error_message(r);

	switch (r) {
	case IMAP_PERMISSION_DENIED: return HTTP_FORBIDDEN;
	case IMAP_MAILBOX_NONEXISTENT: return HTTP_NOT_FOUND;
	default: return HTTP_SERVER_ERROR;
	}
    }

    /* Check ACL for current user */
    rights = acl ? cyrus_acl_myrights(httpd_authstate, acl) : 0;
    if ((rights & DACL_READ) != DACL_READ) {
	/* DAV:need-privileges */
	txn->error.precond = DAV_NEED_PRIVS;
	txn->error.resource = txn->req_tgt.path;
	txn->error.rights = DACL_READ;
	return HTTP_FORBIDDEN;
    }

    if (server) {
	/* Remote mailbox */
	struct backend *be;

	be = proxy_findserver(server, &http_protocol, httpd_userid,
			      &backend_cached, NULL, NULL, httpd_in);
	if (!be) return HTTP_UNAVAILABLE;

	return http_pipe_req_resp(be, txn);
    }

    /* Local Mailbox */

    /* Open mailbox for reading */
    if ((r = http_mailbox_open(txn->req_tgt.mboxname, &mailbox, LOCK_SHARED))) {
	syslog(LOG_ERR, "http_mailbox_open(%s) failed: %s",
	       txn->req_tgt.mboxname, error_message(r));
	txn->error.desc = error_message(r);
	ret = HTTP_SERVER_ERROR;
	goto done;
    }

    /* Find message UID for the resource */
    gparams->lookup_resource(*gparams->davdb, txn->req_tgt.mboxname,
			     txn->req_tgt.resource, 0, (void **) &ddata);
    /* XXX  Check errors */

    /* Fetch index record for the resource */
    if (!ddata->imap_uid ||
	mailbox_find_index_record(mailbox, ddata->imap_uid, &record)) {
	ret = HTTP_NOT_FOUND;
	goto done;
    }

    /* Resource length doesn't include RFC 5322 header */
    offset = record.header_size;
    datalen = record.size - offset;

    /* Check any preconditions, including range request */
    txn->flags.ranges = 1;
    resp_body->range.len = datalen;
    etag = message_guid_encode(&record.guid);
    lastmod = record.internaldate;
    precond = gparams->check_precond(txn, (void *) ddata, etag, lastmod);

    switch (precond) {
    case HTTP_OK:
	break;

    case HTTP_PARTIAL:
	/* Set data parameters for range */
	offset += resp_body->range.first;
	datalen = resp_body->range.last - resp_body->range.first + 1;
	break;

    case HTTP_NOT_MODIFIED:
	/* Fill in ETag for 304 response */
	resp_body->etag = etag;

    default:
	/* We failed a precondition - don't perform the request */
	ret = precond;
	goto done;
    }

    /* Fill in Last-Modified, ETag, and Content-Type */
    resp_body->lastmod = lastmod;
    resp_body->etag = etag;
    resp_body->type = gparams->content_type;

    if (txn->meth == METH_GET) {
	/* Load message containing the resource */
	mailbox_map_message(mailbox, record.uid, &msg_base, &msg_size);

	/* iCalendar data in response should not be transformed */
	txn->flags.cc |= CC_NOTRANSFORM;

	data = msg_base + offset;
    }

    write_body(precond, txn, data, datalen);

    if (msg_base)
	mailbox_unmap_message(mailbox, record.uid, &msg_base, &msg_size);

  done:
    if (mailbox) mailbox_unlock_index(mailbox, NULL);

    return ret;
}


/* Perform a MKCOL/MKCALENDAR request */
/*
 * preconditions:
 *   DAV:resource-must-be-null
 *   DAV:need-privileges
 *   DAV:valid-resourcetype
 *   CALDAV:calendar-collection-location-ok
 *   CALDAV:valid-calendar-data (CALDAV:calendar-timezone)
 */
int meth_mkcol(struct transaction_t *txn, void *params)
{
    int ret = 0, r = 0;
    xmlDocPtr indoc = NULL, outdoc = NULL;
    xmlNodePtr root = NULL, instr = NULL;
    xmlNsPtr ns[NUM_NAMESPACE];
    char *partition = NULL;
    struct proppatch_ctx pctx;
    struct mkcol_params *mparams = (struct mkcol_params *) params;

    memset(&pctx, 0, sizeof(struct proppatch_ctx));

    /* Response should not be cached */
    txn->flags.cc |= CC_NOCACHE;

    /* Make sure its a DAV resource */
    if (!(txn->req_tgt.allow & ALLOW_WRITE)) return HTTP_NOT_ALLOWED; 

    /* Parse the path */
    if ((r = mparams->parse_path(&txn->req_tgt, &txn->error.desc))) {
	txn->error.precond = CALDAV_LOCATION_OK;
	return HTTP_FORBIDDEN;
    }

    /* Make sure its a home-set collection */
    if (!txn->req_tgt.collection || txn->req_tgt.resource) {
	txn->error.precond = CALDAV_LOCATION_OK;
	return HTTP_FORBIDDEN;
    }

    /* Check if we are allowed to create the mailbox */
    r = mboxlist_createmailboxcheck(txn->req_tgt.mboxname, 0, NULL,
				    httpd_userisadmin || httpd_userisproxyadmin,
				    httpd_userid, httpd_authstate,
				    NULL, &partition, 0);

    if (r == IMAP_PERMISSION_DENIED) return HTTP_FORBIDDEN;
    else if (r == IMAP_MAILBOX_EXISTS) {
	txn->error.precond = DAV_RSRC_EXISTS;
	return HTTP_FORBIDDEN;
    }
    else if (r) return HTTP_SERVER_ERROR;

    if (!config_partitiondir(partition)) {
	/* Invalid partition, assume its a server (remote mailbox) */
	char *server = partition, *p;
	struct backend *be;

	/* Trim remote partition */
	p = strchr(server, '!');
	if (p) *p++ = '\0';

	be = proxy_findserver(server, &http_protocol, httpd_userid,
			      &backend_cached, NULL, NULL, httpd_in);
	if (!be) return HTTP_UNAVAILABLE;

	return http_pipe_req_resp(be, txn);
    }

    /* Local Mailbox */

    /* Parse the MKCOL/MKCALENDAR body, if exists */
    ret = parse_xml_body(txn, &root);
    if (ret) goto done;

    if (root) {
	/* Check for correct root element */
	indoc = root->doc;

	if (xmlStrcmp(root->name, BAD_CAST mparams->xml_req)) {
	    txn->error.desc = "Incorrect root element in XML request\r\n";
	    return HTTP_BAD_MEDIATYPE;
	}

	instr = root->children;
    }

    if (instr) {
	/* Start construction of our mkcol/mkcalendar response */
	root = init_xml_response(mparams->xml_resp, mparams->xml_ns, root, ns);
	if (!root) {
	    ret = HTTP_SERVER_ERROR;
	    txn->error.desc = "Unable to create XML response\r\n";
	    goto done;
	}

	outdoc = root->doc;

	/* Populate our proppatch context */
	pctx.req_tgt = &txn->req_tgt;
	pctx.meth = txn->meth;
	pctx.mailboxname = txn->req_tgt.mboxname;
	pctx.root = root;
	pctx.ns = ns;
	pctx.tid = NULL;
	pctx.errstr = &txn->error.desc;
	pctx.ret = &r;

	/* Execute the property patch instructions */
	ret = do_proppatch(&pctx, instr);

	if (ret || r) {
	    /* Something failed.  Abort the txn and change the OK status */
	    annotatemore_abort(pctx.tid);

	    if (!ret) {
		/* Output the XML response */
		xml_response(HTTP_FORBIDDEN, txn, outdoc);
		ret = 0;
	    }

	    goto done;
	}
    }

    /* Create the mailbox */
    r = mboxlist_createmailbox(txn->req_tgt.mboxname, mparams->mbtype, partition, 
			       httpd_userisadmin || httpd_userisproxyadmin,
			       httpd_userid, httpd_authstate,
			       0, 0, 0);

    if (!r) ret = HTTP_CREATED;
    else if (r == IMAP_PERMISSION_DENIED) ret = HTTP_FORBIDDEN;
    else if (r == IMAP_MAILBOX_EXISTS) {
	txn->error.precond = DAV_RSRC_EXISTS;
	ret = HTTP_FORBIDDEN;
    }
    else if (r) {
	txn->error.desc = error_message(r);
	ret = HTTP_SERVER_ERROR;
    }

    if (instr) {
	if (r) {
	    /* Failure.  Abort the txn */
	    annotatemore_abort(pctx.tid);
	}
	else {
	    /* Success.  Commit the txn */
	    annotatemore_commit(pctx.tid);
	}
    }

  done:
    buf_free(&pctx.buf);

    if (outdoc) xmlFreeDoc(outdoc);
    if (indoc) xmlFreeDoc(indoc);

    return ret;
}


/* dav_foreach() callback to find props on a resource */
int propfind_by_resource(void *rock, void *data)
{
    struct propfind_ctx *fctx = (struct propfind_ctx *) rock;
    struct dav_data *ddata = (struct dav_data *) data;
    struct index_record record;
    char *p;
    size_t len;
    int r, ret = 0;

    /* Append resource name to URL path */
    if (!fctx->req_tgt->resource) {
	len = strlen(fctx->req_tgt->path);
	p = fctx->req_tgt->path + len;
    }
    else {
	p = fctx->req_tgt->resource;
	len = p - fctx->req_tgt->path;
    }

    if (p[-1] != '/') {
	*p++ = '/';
	len++;
    }
    strlcpy(p, ddata->resource, MAX_MAILBOX_PATH - len);
    fctx->req_tgt->resource = p;
    fctx->req_tgt->reslen = strlen(p);

    fctx->data = data;
    if (ddata->imap_uid && !fctx->record) {
	/* Fetch index record for the resource */
	r = mailbox_find_index_record(fctx->mailbox, ddata->imap_uid,
				      &record);
	/* XXX  Check errors */

	fctx->record = r ? NULL : &record;
    }

    if (!ddata->imap_uid || !fctx->record) {
	/* Add response for missing target */
	ret = xml_add_response(fctx, HTTP_NOT_FOUND);
    }
    else {
	int add_it = 1;

	if (fctx->filter) add_it = fctx->filter(fctx, data);

	if (add_it) {
	    /* Add response for target */
	    ret = xml_add_response(fctx, 0);
	}
    }

    if (fctx->msg_base) {
	mailbox_unmap_message(fctx->mailbox, ddata->imap_uid,
			      &fctx->msg_base, &fctx->msg_size);
    }
    fctx->msg_base = NULL;
    fctx->msg_size = 0;
    fctx->record = NULL;
    fctx->data = NULL;

    return ret;
}


/* mboxlist_findall() callback to find props on a collection */
int propfind_by_collection(char *mboxname, int matchlen,
			   int maycreate __attribute__((unused)),
			   void *rock)
{
    struct propfind_ctx *fctx = (struct propfind_ctx *) rock;
    struct mboxlist_entry mbentry;
    struct mailbox *mailbox = NULL;
    char *p;
    size_t len;
    int r = 0, rights, root;

    /* If this function is called outside of mboxlist_findall()
       with matchlen == 0, this is the root resource of the PROPFIND */
    root = !matchlen;

    /* Check ACL on mailbox for current user */
    if ((r = mboxlist_lookup(mboxname, &mbentry, NULL))) {
	syslog(LOG_INFO, "mboxlist_lookup(%s) failed: %s",
	       mboxname, error_message(r));
	*fctx->errstr = error_message(r);
	*fctx->ret = HTTP_SERVER_ERROR;
	goto done;
    }

    rights = mbentry.acl ? cyrus_acl_myrights(httpd_authstate, mbentry.acl) : 0;
    if ((rights & fctx->reqd_privs) != fctx->reqd_privs) goto done;

    /* Open mailbox for reading */
    if ((r = mailbox_open_irl(mboxname, &mailbox))) {
	syslog(LOG_INFO, "mailbox_open_irl(%s) failed: %s",
	       mboxname, error_message(r));
	*fctx->errstr = error_message(r);
	*fctx->ret = HTTP_SERVER_ERROR;
	goto done;
    }

    fctx->mailbox = mailbox;
    fctx->record = NULL;

    if (!fctx->req_tgt->resource) {
	/* Append collection name to URL path */
	if (!fctx->req_tgt->collection) {
	    len = strlen(fctx->req_tgt->path);
	    p = fctx->req_tgt->path + len;
	}
	else {
	    p = fctx->req_tgt->collection;
	    len = p - fctx->req_tgt->path;
	}

	if (p[-1] != '/') {
	    *p++ = '/';
	    len++;
	}
	strlcpy(p, strrchr(mboxname, '.') + 1, MAX_MAILBOX_PATH - len);
	strlcat(p, "/", MAX_MAILBOX_PATH - len - 1);
	fctx->req_tgt->collection = p;
	fctx->req_tgt->collen = strlen(p);

	/* If not filtering by calendar resource, and not excluding root,
	   add response for collection */
	if (!fctx->filter &&
	    (!root || (fctx->depth == 1) || !(fctx->prefer & PREFER_NOROOT)) &&
	    (r = xml_add_response(fctx, 0))) goto done;
    }

    if (fctx->depth > 1) {
	/* Resource(s) */

	if (fctx->req_tgt->resource) {
	    /* Add response for target resource */
	    void *data;

	    /* Find message UID for the resource */
	    fctx->lookup_resource(fctx->davdb,
				  mboxname, fctx->req_tgt->resource, 0, &data);
	    /* XXX  Check errors */

	    r = fctx->proc_by_resource(rock, data);
	}
	else {
	    /* Add responses for all contained resources */
	    fctx->foreach_resource(fctx->davdb, mboxname,
				   fctx->proc_by_resource, rock);

	    /* Started with NULL resource, end with NULL resource */
	    fctx->req_tgt->resource = NULL;
	    fctx->req_tgt->reslen = 0;
	}
    }

  done:
    if (mailbox) mailbox_close(&mailbox);

    return r;
}


/* Perform a PROPFIND request */
int meth_propfind(struct transaction_t *txn, void *params)
{
    int ret = 0, r;
    const char **hdr;
    unsigned depth;
    xmlDocPtr indoc = NULL, outdoc = NULL;
    xmlNodePtr root, cur = NULL;
    xmlNsPtr ns[NUM_NAMESPACE];
    struct propfind_params *fparams = (struct propfind_params *) params;
    struct propfind_ctx fctx;
    struct propfind_entry_list *elist = NULL;

    memset(&fctx, 0, sizeof(struct propfind_ctx));

    /* Make sure its a DAV resource */
    if (!(txn->req_tgt.allow & ALLOW_DAV)) return HTTP_NOT_ALLOWED;

    /* Parse the path */
    if ((r = fparams->parse_path(&txn->req_tgt, &txn->error.desc))) return r;

    /* Check Depth */
    hdr = spool_getheader(txn->req_hdrs, "Depth");
    if (!hdr || !strcmp(hdr[0], "infinity")) {
	depth = 2;
    }
    else if (hdr && ((sscanf(hdr[0], "%u", &depth) != 1) || (depth > 1))) {
	txn->error.desc = "Illegal Depth value\r\n";
	return HTTP_BAD_REQUEST;
    }

    if ((txn->req_tgt.allow & ALLOW_WRITE) && txn->req_tgt.user) {
	char *server, *acl;
	int rights;

	/* Locate the mailbox */
	if ((r = http_mlookup(txn->req_tgt.mboxname, &server, &acl, NULL))) {
	    syslog(LOG_ERR, "mlookup(%s) failed: %s",
		   txn->req_tgt.mboxname, error_message(r));
	    txn->error.desc = error_message(r);

	    switch (r) {
	    case IMAP_PERMISSION_DENIED: return HTTP_FORBIDDEN;
	    case IMAP_MAILBOX_NONEXISTENT: return HTTP_NOT_FOUND;
	    default: return HTTP_SERVER_ERROR;
	    }
	}

	/* Check ACL for current user */
	rights = acl ? cyrus_acl_myrights(httpd_authstate, acl) : 0;
	if ((rights & DACL_READ) != DACL_READ) {
	    /* DAV:need-privileges */
	    txn->error.precond = DAV_NEED_PRIVS;
	    txn->error.resource = txn->req_tgt.path;
	    txn->error.rights = DACL_READ;
	    ret = HTTP_FORBIDDEN;
	    goto done;
	}

	if (server) {
	    /* Remote mailbox */
	    struct backend *be;

	    be = proxy_findserver(server, &http_protocol, httpd_userid,
				  &backend_cached, NULL, NULL, httpd_in);
	    if (!be) return HTTP_UNAVAILABLE;

	    return http_pipe_req_resp(be, txn);
	}

	/* Local Mailbox */
    }

    /* Principal or Local Mailbox */

    /* Normalize depth so that:
     * 0 = home-set collection, 1+ = calendar collection, 2+ = calendar resource
     */
    if (txn->req_tgt.collection) depth++;
    if (txn->req_tgt.resource) depth++;

    /* Parse the PROPFIND body, if exists */
    ret = parse_xml_body(txn, &root);
    if (ret) goto done;

    if (!root) {
	/* XXX allprop request */
    }
    else {
	indoc = root->doc;

	/* XXX  Need to support propname request too! */

	/* Make sure its a propfind element */
	if (xmlStrcmp(root->name, BAD_CAST "propfind")) {
	    txn->error.desc = "Missing propfind element in PROFIND request\r\n";
	    ret = HTTP_BAD_REQUEST;
	    goto done;
	}

	/* Find child element of propfind */
	for (cur = root->children;
	     cur && cur->type != XML_ELEMENT_NODE; cur = cur->next);

	/* Make sure its a prop element */
	/* XXX  TODO: Check for allprop and propname too */
	if (!cur || xmlStrcmp(cur->name, BAD_CAST "prop")) {
	    ret = HTTP_BAD_REQUEST;
	    goto done;
	}
    }

    /* Start construction of our multistatus response */
    if (!(root = init_xml_response("multistatus", NS_DAV, root, ns))) {
	ret = HTTP_SERVER_ERROR;
	txn->error.desc = "Unable to create XML response\r\n";
	goto done;
    }

    outdoc = root->doc;

    /* Populate our propfind context */
    fctx.req_tgt = &txn->req_tgt;
    fctx.depth = depth;
    fctx.prefer = get_preferences(txn);
    fctx.userid = httpd_userid;
    fctx.userisadmin = httpd_userisadmin;
    fctx.authstate = httpd_authstate;
    fctx.mailbox = NULL;
    fctx.record = NULL;
    fctx.reqd_privs = DACL_READ;
    fctx.filter = NULL;
    fctx.filter_crit = NULL;
    if (fparams->davdb) {
	fctx.davdb = *fparams->davdb;
	fctx.lookup_resource = fparams->lookup;
	fctx.foreach_resource = fparams->foreach;
    }
    fctx.proc_by_resource = &propfind_by_resource;
    fctx.elist = NULL;
    fctx.root = root;
    fctx.ns = ns;
    fctx.errstr = &txn->error.desc;
    fctx.ret = &ret;
    fctx.fetcheddata = 0;

    /* Parse the list of properties and build a list of callbacks */
    preload_proplist(cur->children, &fctx);

    if (!txn->req_tgt.collection &&
	(!depth || !(fctx.prefer & PREFER_NOROOT))) {
	/* Add response for principal or home-set collection */
	struct mailbox *mailbox = NULL;

	if (*txn->req_tgt.mboxname) {
	    /* Open mailbox for reading */
	    if ((r = mailbox_open_irl(txn->req_tgt.mboxname, &mailbox))) {
		syslog(LOG_INFO, "mailbox_open_irl(%s) failed: %s",
		       txn->req_tgt.mboxname, error_message(r));
		txn->error.desc = error_message(r);
		ret = HTTP_SERVER_ERROR;
		goto done;
	    }
	    fctx.mailbox = mailbox;
	}

	xml_add_response(&fctx, 0);

	mailbox_close(&mailbox);
    }

    if (depth > 0) {
	/* Calendar collection(s) */

	if (txn->req_tgt.collection) {
	    /* Add response for target calendar collection */
	    propfind_by_collection(txn->req_tgt.mboxname, 0, 0, &fctx);
	}
	else {
	    /* Add responses for all contained calendar collections */
	    strlcat(txn->req_tgt.mboxname, ".%", sizeof(txn->req_tgt.mboxname));
	    r = mboxlist_findall(NULL,  /* internal namespace */
				 txn->req_tgt.mboxname, 1, httpd_userid, 
				 httpd_authstate, propfind_by_collection, &fctx);
	}

	ret = *fctx.ret;
    }

    /* Output the XML response */
    if (!ret) {
	/* iCalendar data in response should not be transformed */
	if (fctx.fetcheddata) txn->flags.cc |= CC_NOTRANSFORM;

	xml_response(HTTP_MULTI_STATUS, txn, outdoc);
    }

  done:
    /* Free the entry list */
    elist = fctx.elist;
    while (elist) {
	struct propfind_entry_list *freeme = elist;
	elist = elist->next;
	free(freeme);
    }

    buf_free(&fctx.buf);

    if (outdoc) xmlFreeDoc(outdoc);
    if (indoc) xmlFreeDoc(indoc);

    return ret;
}


/* Perform a PROPPATCH request
 *
 * preconditions:
 *   DAV:cannot-modify-protected-property
 *   CALDAV:valid-calendar-data (CALDAV:calendar-timezone)
 */
int meth_proppatch(struct transaction_t *txn,  void *params)
{
    int ret = 0, r = 0, rights;
    xmlDocPtr indoc = NULL, outdoc = NULL;
    xmlNodePtr root, instr, resp;
    xmlNsPtr ns[NUM_NAMESPACE];
    char *server, *acl;
    struct proppatch_ctx pctx;
    struct proppatch_params *pparams = (struct proppatch_params *) params;

    memset(&pctx, 0, sizeof(struct proppatch_ctx));

    /* Response should not be cached */
    txn->flags.cc |= CC_NOCACHE;

    /* Make sure its a DAV resource */
    if (!(txn->req_tgt.allow & ALLOW_WRITE)) return HTTP_NOT_ALLOWED;

    /* Parse the path */
    if ((r = pparams->parse_path(&txn->req_tgt, &txn->error.desc))) return r;

    /* Make sure its a collection */
    if (txn->req_tgt.resource) {
	txn->error.desc =
	    "Properties can only be updated on collections\r\n";
	return HTTP_FORBIDDEN;
    }

    /* Locate the mailbox */
    if ((r = http_mlookup(txn->req_tgt.mboxname, &server, &acl, NULL))) {
	syslog(LOG_ERR, "mlookup(%s) failed: %s",
	       txn->req_tgt.mboxname, error_message(r));
	txn->error.desc = error_message(r);

	switch (r) {
	case IMAP_PERMISSION_DENIED: return HTTP_FORBIDDEN;
	case IMAP_MAILBOX_NONEXISTENT: return HTTP_NOT_FOUND;
	default: return HTTP_SERVER_ERROR;
	}
    }

    /* Check ACL for current user */
    rights = acl ? cyrus_acl_myrights(httpd_authstate, acl) : 0;
    if (!(rights & DACL_WRITEPROPS)) {
	/* DAV:need-privileges */
	txn->error.precond = DAV_NEED_PRIVS;
	txn->error.resource = txn->req_tgt.path;
	txn->error.rights = DACL_WRITEPROPS;
	return HTTP_FORBIDDEN;
    }

    if (server) {
	/* Remote mailbox */
	struct backend *be;

	be = proxy_findserver(server, &http_protocol, httpd_userid,
			      &backend_cached, NULL, NULL, httpd_in);
	if (!be) return HTTP_UNAVAILABLE;

	return http_pipe_req_resp(be, txn);
    }

    /* Local Mailbox */

    /* Parse the PROPPATCH body */
    ret = parse_xml_body(txn, &root);
    if (!root) {
	txn->error.desc = "Missing request body\r\n";
	return HTTP_BAD_REQUEST;
    }
    if (ret) goto done;

    indoc = root->doc;

    /* Make sure its a propertyupdate element */
    if (xmlStrcmp(root->name, BAD_CAST "propertyupdate")) {
	txn->error.desc =
	    "Missing propertyupdate element in PROPPATCH request\r\n";
	return HTTP_BAD_REQUEST;
    }
    instr = root->children;

    /* Start construction of our multistatus response */
    if (!(root = init_xml_response("multistatus", NS_DAV, root, ns))) {
	ret = HTTP_SERVER_ERROR;
	txn->error.desc = "Unable to create XML response\r\n";
	goto done;
    }

    outdoc = root->doc;

    /* Add a response tree to 'root' for the specified href */
    resp = xmlNewChild(root, NULL, BAD_CAST "response", NULL);
    if (!resp) syslog(LOG_ERR, "new child response failed");
    xmlNewChild(resp, NULL, BAD_CAST "href", BAD_CAST txn->req_tgt.path);

    /* Populate our proppatch context */
    pctx.req_tgt = &txn->req_tgt;
    pctx.meth = txn->meth;
    pctx.mailboxname = txn->req_tgt.mboxname;
    pctx.root = resp;
    pctx.ns = ns;
    pctx.tid = NULL;
    pctx.errstr = &txn->error.desc;
    pctx.ret = &r;

    /* Execute the property patch instructions */
    ret = do_proppatch(&pctx, instr);

    if (ret || r) {
	/* Something failed.  Abort the txn and change the OK status */
	annotatemore_abort(pctx.tid);

	if (ret) goto done;
    }
    else {
	/* Success.  Commit the txn */
	annotatemore_commit(pctx.tid);
    }

    /* Output the XML response */
    if (!ret) {
	if (get_preferences(txn) & PREFER_MIN) ret = HTTP_OK;
	else xml_response(HTTP_MULTI_STATUS, txn, outdoc);
    }

  done:
    buf_free(&pctx.buf);

    if (outdoc) xmlFreeDoc(outdoc);
    if (indoc) xmlFreeDoc(indoc);

    return ret;
}


/* Compare modseq in index maps -- used for sorting */
static int map_modseq_cmp(const struct index_map *m1,
			  const struct index_map *m2)
{
    if (m1->record.modseq < m2->record.modseq) return -1;
    if (m1->record.modseq > m2->record.modseq) return 1;
    return 0;
}


int report_sync_col(struct transaction_t *txn,
		    xmlNodePtr inroot, struct propfind_ctx *fctx)
{
    int ret = 0, r, userflag;
    struct mailbox *mailbox = NULL;
    uint32_t uidvalidity = 0;
    modseq_t syncmodseq = 0, highestmodseq;
    uint32_t limit = -1, recno, nresp;
    xmlNodePtr node;
    struct index_state istate;
    struct index_record *record;
    char tokenuri[MAX_MAILBOX_PATH+1];

    /* XXX  Handle Depth (cal-home-set at toplevel) */

    istate.map = NULL;

    /* Open mailbox for reading */
    if ((r = http_mailbox_open(txn->req_tgt.mboxname, &mailbox, LOCK_SHARED))) {
	syslog(LOG_ERR, "http_mailbox_open(%s) failed: %s",
	       txn->req_tgt.mboxname, error_message(r));
	txn->error.desc = error_message(r);
	ret = HTTP_SERVER_ERROR;
	goto done;
    }

    fctx->mailbox = mailbox;

    highestmodseq = mailbox->i.highestmodseq;
    if (mailbox_user_flag(mailbox, DFLAG_UNBIND, &userflag)) userflag = -1;

    /* Parse children element of report */
    for (node = inroot->children; node; node = node->next) {
	xmlNodePtr node2;
	xmlChar *str;
	if (node->type == XML_ELEMENT_NODE) {
	    if (!xmlStrcmp(node->name, BAD_CAST "sync-token") &&
		(str = xmlNodeListGetString(inroot->doc, node->children, 1))) {
		if (xmlStrncmp(str, BAD_CAST XML_NS_CYRUS "sync/",
			       strlen(XML_NS_CYRUS "sync/")) ||
		    (sscanf(strrchr((char *) str, '/') + 1,
			    "%u-" MODSEQ_FMT,
			    &uidvalidity, &syncmodseq) != 2) ||
		    !syncmodseq ||
		    (uidvalidity != mailbox->i.uidvalidity) ||
		    (syncmodseq < mailbox->i.deletedmodseq) ||
		    (syncmodseq > highestmodseq)) {
		    /* DAV:valid-sync-token */
		    *fctx->errstr = "Invalid sync-token";
		    ret = HTTP_FORBIDDEN;
		    goto done;
		}
	    }
	    if (!xmlStrcmp(node->name, BAD_CAST "sync-level") &&
		(str = xmlNodeListGetString(inroot->doc, node->children, 1))) {
		if (!strcmp((char *) str, "infinity")) {
		    *fctx->errstr =
			"This server DOES NOT support infinite depth requests";
		    ret = HTTP_SERVER_ERROR;
		    goto done;
		}
		else if ((sscanf((char *) str, "%u", &fctx->depth) != 1) ||
			 (fctx->depth != 1)) {
		    *fctx->errstr = "Illegal sync-level";
		    ret = HTTP_BAD_REQUEST;
		    goto done;
		}
	    }
	    if (!xmlStrcmp(node->name, BAD_CAST "limit")) {
		for (node2 = node->children; node2; node2 = node2->next) {
		    if ((node2->type == XML_ELEMENT_NODE) &&
			!xmlStrcmp(node2->name, BAD_CAST "nresults") &&
			(!(str = xmlNodeListGetString(inroot->doc,
						      node2->children, 1)) ||
			 (sscanf((char *) str, "%u", &limit) != 1))) {
			*fctx->errstr = "Invalid limit";
			ret = HTTP_FORBIDDEN;
			goto done;
		    }
		}
	    }
	}
    }

    /* Check Depth */
    if (!fctx->depth) {
	*fctx->errstr = "Illegal sync-level";
	ret = HTTP_BAD_REQUEST;
	goto done;
    }


    /* Construct array of records for sorting and/or fetching cached header */
    istate.mailbox = mailbox;
    istate.map = xzmalloc(mailbox->i.num_records *
			  sizeof(struct index_map));

    /* Find which resources we need to report */
    for (nresp = 0, recno = 1; recno <= mailbox->i.num_records; recno++) {

	record = &istate.map[nresp].record;
	if (mailbox_read_index_record(mailbox, recno, record)) {
	    /* XXX  Corrupted record?  Should we bail? */
	    continue;
	}

	if (record->modseq <= syncmodseq) {
	    /* Resource not added/removed since last sync */
	    continue;
	}

	if ((userflag >= 0) &&
	    record->user_flags[userflag / 32] & (1 << (userflag & 31))) {
	    /* Resource replaced by a PUT, COPY, or MOVE - ignore it */
	    continue;
	}

	if (!syncmodseq && (record->system_flags & FLAG_EXPUNGED)) {
	    /* Initial sync - ignore unmapped resources */
	    continue;
	}

	nresp++;
    }

    if (limit < nresp) {
	/* Need to truncate the responses */
	struct index_map *map = istate.map;

	/* Sort the response records by modseq */
	qsort(map, nresp, sizeof(struct index_map),
	      (int (*)(const void *, const void *)) &map_modseq_cmp);

	/* Our last response MUST be the last record with its modseq */
	for (nresp = limit;
	     nresp && map[nresp-1].record.modseq == map[nresp].record.modseq;
	     nresp--);

	if (!nresp) {
	    /* DAV:number-of-matches-within-limits */
	    *fctx->errstr = "Unable to truncate results";
	    ret = HTTP_FORBIDDEN;  /* HTTP_NO_STORAGE ? */
	    goto done;
	}

	/* highestmodseq will be modseq of last record we return */
	highestmodseq = map[nresp-1].record.modseq;

	/* Tell client we truncated the responses */
	xml_add_response(fctx, HTTP_NO_STORAGE);
    }

    /* Report the resources within the client requested limit (if any) */
    for (recno = 1; recno <= nresp; recno++) {
	char *p, *resource = NULL;
	struct caldav_data cdata;

	record = &istate.map[recno-1].record;

	/* Get resource filename from Content-Disposition header */
	if ((p = index_getheader(&istate, recno, "Content-Disposition"))) {
	    resource = strstr(p, "filename=") + 9;
	}
	if (!resource) continue;  /* No filename */

	if (*resource == '\"') {
	    resource++;
	    if ((p = strchr(resource, '\"'))) *p = '\0';
	}
	else if ((p = strchr(resource, ';'))) *p = '\0';

	memset(&cdata, 0, sizeof(struct caldav_data));
	cdata.dav.resource = resource;

	if (record->system_flags & FLAG_EXPUNGED) {
	    /* report as NOT FOUND
	       IMAP UID of 0 will cause index record to be ignored
	       propfind_by_resource() will append our resource name */
	    propfind_by_resource(fctx, &cdata);
	}
	else {
	    fctx->record = record;
	    cdata.dav.imap_uid = record->uid;
	    propfind_by_resource(fctx, &cdata);
	}
    }

    /* Add sync-token element */
    snprintf(tokenuri, MAX_MAILBOX_PATH,
	     XML_NS_CYRUS "sync/%u-" MODSEQ_FMT,
	     mailbox->i.uidvalidity, highestmodseq);
    xmlNewChild(fctx->root, NULL, BAD_CAST "sync-token", BAD_CAST tokenuri);

  done:
    if (istate.map) free(istate.map);
    if (mailbox) mailbox_unlock_index(mailbox, NULL);

    return ret;
}


/* Perform a REPORT request */
int meth_report(struct transaction_t *txn, void *params)
{
    int ret = 0, r;
    const char **hdr;
    unsigned depth = 0;
    xmlNodePtr inroot = NULL, outroot = NULL, cur, prop = NULL;
    const struct report_type_t *report = NULL;
    xmlNsPtr ns[NUM_NAMESPACE];
    struct propfind_ctx fctx;
    struct propfind_entry_list *elist = NULL;
    struct report_params *rparams = (struct report_params *) params;

    memset(&fctx, 0, sizeof(struct propfind_ctx));

    /* Make sure its a DAV resource */
    if (!(txn->req_tgt.allow & ALLOW_DAV)) return HTTP_NOT_ALLOWED; 

    /* Parse the path */
    if ((r = rparams->parse_path(&txn->req_tgt, &txn->error.desc))) return r;

    /* Check Depth */
    if ((hdr = spool_getheader(txn->req_hdrs, "Depth"))) {
	if (!strcmp(hdr[0], "infinity")) {
	    depth = 2;
	}
	else if ((sscanf(hdr[0], "%u", &depth) != 1) || (depth > 1)) {
	    txn->error.desc = "Illegal Depth value\r\n";
	    return HTTP_BAD_REQUEST;
	}
    }

    /* Normalize depth so that:
     * 0 = home-set collection, 1+ = calendar collection, 2+ = calendar resource
     */
    if (txn->req_tgt.collection) depth++;
    if (txn->req_tgt.resource) depth++;

    /* Parse the REPORT body */
    ret = parse_xml_body(txn, &inroot);
    if (!inroot) {
	txn->error.desc = "Missing request body\r\n";
	return HTTP_BAD_REQUEST;
    }
    if (ret) goto done;

    /* Check the report type against our supported list */
    for (report = rparams->reports; report && report->name; report++) {
	if (!xmlStrcmp(inroot->name, BAD_CAST report->name)) break;
    }
    if (!report || !report->name) {
	syslog(LOG_WARNING, "REPORT %s", inroot->name);
	/* DAV:supported-report */
	txn->error.precond = DAV_SUPP_REPORT;
	ret = HTTP_FORBIDDEN;
	goto done;
    }

    if (report->flags & REPORT_NEED_MBOX) {
	char *server, *acl;
	int rights;

	/* Locate the mailbox */
	if ((r = http_mlookup(txn->req_tgt.mboxname, &server, &acl, NULL))) {
	    syslog(LOG_ERR, "mlookup(%s) failed: %s",
		   txn->req_tgt.mboxname, error_message(r));
	    txn->error.desc = error_message(r);

	    switch (r) {
	    case IMAP_PERMISSION_DENIED: ret = HTTP_FORBIDDEN;
	    case IMAP_MAILBOX_NONEXISTENT: ret = HTTP_NOT_FOUND;
	    default: ret = HTTP_SERVER_ERROR;
	    }
	    goto done;
	}

	/* Check ACL for current user */
	rights = acl ? cyrus_acl_myrights(httpd_authstate, acl) : 0;
	if ((rights & report->reqd_privs) != report->reqd_privs) {
	    if (report->reqd_privs == DACL_READFB) ret = HTTP_NOT_FOUND;
	    else {
		/* DAV:need-privileges */
		txn->error.precond = DAV_NEED_PRIVS;
		txn->error.resource = txn->req_tgt.path;
		txn->error.rights = report->reqd_privs;
		ret = HTTP_FORBIDDEN;
	    }
	    goto done;
	}

	if (server) {
	    /* Remote mailbox */
	    struct backend *be;

	    be = proxy_findserver(server, &http_protocol, httpd_userid,
				  &backend_cached, NULL, NULL, httpd_in);
	    if (!be) ret = HTTP_UNAVAILABLE;
	    else ret = http_pipe_req_resp(be, txn);
	    goto done;
	}

	/* Local Mailbox */
    }

    /* Principal or Local Mailbox */

    /* Parse children element of report */
    for (cur = inroot->children; cur; cur = cur->next) {
	if (cur->type == XML_ELEMENT_NODE) {
	    if (!xmlStrcmp(cur->name, BAD_CAST "allprop")) {
		syslog(LOG_WARNING, "REPORT %s w/allprop", report->name);
		txn->error.desc = "Unsupported REPORT option <allprop>\r\n";
		ret = HTTP_NOT_IMPLEMENTED;
		goto done;
	    }
	    else if (!xmlStrcmp(cur->name, BAD_CAST "propname")) {
		syslog(LOG_WARNING, "REPORT %s w/propname", report->name);
		txn->error.desc = "Unsupported REPORT option <propname>\r\n";
		ret = HTTP_NOT_IMPLEMENTED;
		goto done;
	    }
	    else if (!xmlStrcmp(cur->name, BAD_CAST "prop")) {
		prop = cur;
		break;
	    }
	}
    }

    if (!prop && (report->flags & REPORT_NEED_PROPS)) {
	txn->error.desc = "Missing <prop> element in REPORT\r\n";
	ret = HTTP_BAD_REQUEST;
	goto done;
    }

    /* Start construction of our multistatus response */
    if ((report->flags & REPORT_MULTISTATUS) &&
	!(outroot = init_xml_response("multistatus", NS_DAV, inroot, ns))) {
	txn->error.desc = "Unable to create XML response\r\n";
	ret = HTTP_SERVER_ERROR;
	goto done;
    }

    /* Populate our propfind context */
    fctx.req_tgt = &txn->req_tgt;
    fctx.depth = depth;
    fctx.prefer = get_preferences(txn);
    fctx.userid = httpd_userid;
    fctx.userisadmin = httpd_userisadmin;
    fctx.authstate = httpd_authstate;
    fctx.mailbox = NULL;
    fctx.record = NULL;
    fctx.reqd_privs = report->reqd_privs;
    fctx.elist = NULL;
    fctx.root = outroot;
    fctx.ns = ns;
    fctx.errstr = &txn->error.desc;
    fctx.ret = &ret;
    fctx.fetcheddata = 0;

    /* Parse the list of properties and build a list of callbacks */
    if (prop) preload_proplist(prop->children, &fctx);

    /* Process the requested report */
    ret = (*report->proc)(txn, inroot, &fctx);

    /* Output the XML response */
    if (!ret && outroot) {
	/* iCalendar data in response should not be transformed */
	if (fctx.fetcheddata) txn->flags.cc |= CC_NOTRANSFORM;

	xml_response(HTTP_MULTI_STATUS, txn, outroot->doc);
    }

  done:
    /* Free the entry list */
    elist = fctx.elist;
    while (elist) {
	struct propfind_entry_list *freeme = elist;
	elist = elist->next;
	free(freeme);
    }

    buf_free(&fctx.buf);

    if (inroot) xmlFreeDoc(inroot->doc);
    if (outroot) xmlFreeDoc(outroot->doc);

    return ret;
}