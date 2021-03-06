/* chk_cyrus.c: cyrus mailstore consistancy checker
 *
 * Copyright (c) 1994-2008 Carnegie Mellon University.  All rights reserved.
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
 * $Id: chk_cyrus.c,v 1.20 2010/01/06 17:01:30 murch Exp $
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <syslog.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "exitcodes.h"
#include "index.h"
#include "global.h"
#include "mboxlist.h"
#include "mailbox.h"
#include "map.h"
#include "xmalloc.h"
#include "xstrlcpy.h"
#include "xstrlcat.h"

/* config.c stuff */
const int config_need_data = CONFIG_NEED_PARTITION_DATA;

void usage(void)
{
    fprintf(stderr, "chk_cyrus [-C <altconfig>] partition\n");
    exit(-1);
}

const char *check_part = NULL; /* partition we are checking */

static int chkmbox(char *name,
		   int matchlen __attribute__((unused)),
		   int maycreate __attribute__((unused)),
		   void *rock __attribute__((unused))) 
{
    int r;
    struct mboxlist_entry mbentry;

    r = mboxlist_lookup(name, &mbentry, NULL);

    /* xxx reserved mailboxes? */

    if (r) {
	fprintf(stderr, "bad mailbox %s in chkmbox\n", name);
	fatal("fatal error",EC_TEMPFAIL);
    }

    /* are we on the partition we are checking? */
    if (check_part && strcmp(mbentry.partition, check_part))
	return 0;

    fprintf(stderr, "checking: %s\n", name);

    mailbox_reconstruct(name, 0); /* no changes allowed */

    return 0;
}

int main(int argc, char **argv) 
{
    char *alt_config = NULL;
    char pattern[2] = { '*', '\0' };
    const char *mailbox = NULL;

    extern char *optarg;
    int opt;

    while ((opt = getopt(argc, argv, "C:P:M:")) != EOF) {
	switch (opt) {
	case 'C': /* alt config file */
	    alt_config = optarg;
	    break;

	case 'P':
	    if(mailbox) {
		usage();
		exit(EC_USAGE);
	    }
	    check_part = optarg;
	    break;

	case 'M':
	    if(check_part) {
		usage();
		exit(EC_USAGE);
	    }
	    mailbox = optarg;
	    break;

	default:
	    usage();
	    /* NOTREACHED */
	}
    }

    cyrus_init(alt_config, "chk_cyrus", 0);

    mboxlist_init(0);
    mboxlist_open(NULL);

    if(mailbox) {
	fprintf(stderr, "Examining mailbox: %s\n", mailbox);
	chkmbox((char *)mailbox,0,0,NULL);
    } else {
	fprintf(stderr, "Examining partition: %s\n",
		(check_part ? check_part : "ALL PARTITIONS"));
	
	/* build a list of mailboxes - we're using internal names here */
	mboxlist_findall(NULL, pattern, 1, NULL,
			 NULL, chkmbox, NULL);
    }
    
    mboxlist_close();
    mboxlist_done();

    cyrus_done();
    
    return 0;
}
