/*
 * Copyright (c) 2007-2012, Vsevolod Stakhov
 * All rights reserved.

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer. Redistributions in binary form
 * must reproduce the above copyright notice, this list of conditions and the
 * following disclaimer in the documentation and/or other materials provided with
 * the distribution. Neither the name of the author nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.

 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syslog.h>
#include <sys/mman.h>

#include "spf2/spf.h"
#include "cfg_file.h"
#include "spf.h"
#include "rmilter.h"

/* Defined in rmilter.c */
extern int my_strcmp (const void *, const void *);

int
spf_check(struct mlfi_priv *priv, struct config_file *cfg)
{
	char *helo = priv->priv_helo;
	char *fromp = priv->priv_from;
	SPF_server_t *spf_server;
	SPF_request_t *spf_request;
	SPF_response_t *spf_response;
	char from[NS_MAXDNAME + 1];
	int res, result = 0;
	char *domain_pos;
	size_t len;

	/*
	 * And the enveloppe source e-mail
	 */
	if (fromp[0] == '<')
		fromp++; /* strip leading < */
	strlcpy (from, fromp, NS_MAXDNAME);
	len = strlen(from);
	if (fromp[len - 1] == '>')
		from[len - 1] = '\0'; /* strip trailing > */
	domain_pos = strchr (from, '@');
	
	/* No domain part in envfrom field - do not make spf check */
	if (domain_pos == NULL) {
		return 1;	
	}
	
	/* Search in spf_domains array */
	domain_pos ++;
	if (! bsearch ((void *) &domain_pos, cfg->spf_domains, cfg->spf_domains_num, 
					sizeof (char *), my_strcmp)) {
		/* Domain not found, stop check */
		return 1;
	}

	if ((spf_server = SPF_server_new (SPF_DNS_CACHE, 0)) == NULL) {
		msg_err ("spf: SPF_server_new failed");
		goto out1;
	}

	if ((spf_request = SPF_request_new (spf_server)) == NULL) {
		msg_err ("spf: SPF_request_new failed");
		goto out2;
	}

	/*
	 * Get the IP address
	 */
	switch (priv->priv_addr.family) {
	case AF_INET:
		res = SPF_request_set_ipv4 (spf_request, priv->priv_addr.addr.sa4.sin_addr);
		break;
	case AF_INET6:
		res = SPF_request_set_ipv6 (spf_request, priv->priv_addr.addr.sa6.sin6_addr);
		break;
	default:
		msg_err ("spf: unknown address family %d", priv->priv_addr.family);
		goto out3;
	}
	if (res != 0) {
		msg_err ("spf: SPF_request_set_ip_str failed");
		goto out3;
	}

	/* HELO string */
	if (SPF_request_set_helo_dom (spf_request, helo) != 0) {
		msg_err ("spf: SPF_request_set_helo_dom failed");
		goto out3;
	}


	if (SPF_request_set_env_from (spf_request, from) != 0) {
		msg_err ("spf: SPF_request_set_env_from failed");
		goto out3;
	}

	/*
	 * Get the SPF result
	 */
	SPF_request_query_mailfrom (spf_request, &spf_response);
	if ((res = SPF_response_result (spf_response)) == SPF_RESULT_PASS) {
		result = 1;
	}
	else {
		result = res;
	}

	SPF_response_free (spf_response);
out3:
	SPF_request_free (spf_request);
out2:
	SPF_server_free (spf_server);
out1:

	return result;
}
