/*
 * Unix SMB/CIFS implementation.
 *
 * Winbind rpc backend functions
 *
 * Copyright (c) 2000-2003 Tim Potter
 * Copyright (c) 2001      Andrew Tridgell
 * Copyright (c) 2005      Volker Lendecke
 * Copyright (c) 2008      Guenther Deschner (pidl conversion)
 * Copyright (c) 2010      Andreas Schneider <asn@samba.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "includes.h"
#include "winbindd.h"
#include "../librpc/gen_ndr/cli_samr.h"
#include "rpc_client/cli_samr.h"
#include "../librpc/gen_ndr/srv_samr.h"
#include "../librpc/gen_ndr/cli_lsa.h"
#include "rpc_client/cli_lsarpc.h"
#include "../librpc/gen_ndr/srv_lsa.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_WINBIND

static NTSTATUS open_internal_samr_pipe(TALLOC_CTX *mem_ctx,
					struct rpc_pipe_client **samr_pipe)
{
	static struct rpc_pipe_client *cli = NULL;
	struct auth_serversupplied_info *server_info = NULL;
	NTSTATUS status;

	if (cli != NULL) {
		goto done;
	}

	if (server_info == NULL) {
		status = make_server_info_system(mem_ctx, &server_info);
		if (!NT_STATUS_IS_OK(status)) {
			DEBUG(0, ("open_samr_pipe: Could not create auth_serversupplied_info: %s\n",
				  nt_errstr(status)));
			return status;
		}
	}

	/* create a samr connection */
	status = rpc_pipe_open_internal(talloc_autofree_context(),
					&ndr_table_samr.syntax_id,
					rpc_samr_dispatch,
					server_info,
					&cli);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0, ("open_samr_pipe: Could not connect to samr_pipe: %s\n",
			  nt_errstr(status)));
		return status;
	}

done:
	if (samr_pipe) {
		*samr_pipe = cli;
	}

	return NT_STATUS_OK;
}

static NTSTATUS open_internal_samr_conn(TALLOC_CTX *mem_ctx,
				        struct winbindd_domain *domain,
				        struct rpc_pipe_client **samr_pipe,
				        struct policy_handle *samr_domain_hnd)
{
	NTSTATUS status;
	struct policy_handle samr_connect_hnd;

	status = open_internal_samr_pipe(mem_ctx, samr_pipe);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	status = rpccli_samr_Connect2((*samr_pipe),
				      mem_ctx,
				      (*samr_pipe)->desthost,
				      SEC_FLAG_MAXIMUM_ALLOWED,
				      &samr_connect_hnd);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	status = rpccli_samr_OpenDomain((*samr_pipe),
					mem_ctx,
					&samr_connect_hnd,
					SEC_FLAG_MAXIMUM_ALLOWED,
					&domain->sid,
					samr_domain_hnd);

	return status;
}

static NTSTATUS open_internal_lsa_pipe(TALLOC_CTX *mem_ctx,
				       struct rpc_pipe_client **lsa_pipe)
{
	static struct rpc_pipe_client *cli = NULL;
	struct auth_serversupplied_info *server_info = NULL;
	NTSTATUS status;

	if (cli != NULL) {
		goto done;
	}

	if (server_info == NULL) {
		status = make_server_info_system(mem_ctx, &server_info);
		if (!NT_STATUS_IS_OK(status)) {
			DEBUG(0, ("open_samr_pipe: Could not create auth_serversupplied_info: %s\n",
				  nt_errstr(status)));
			return status;
		}
	}

	/* create a samr connection */
	status = rpc_pipe_open_internal(talloc_autofree_context(),
					&ndr_table_lsarpc.syntax_id,
					rpc_lsarpc_dispatch,
					server_info,
					&cli);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0, ("open_samr_pipe: Could not connect to samr_pipe: %s\n",
			  nt_errstr(status)));
		return status;
	}

done:
	if (lsa_pipe) {
		*lsa_pipe = cli;
	}

	return NT_STATUS_OK;
}

static NTSTATUS open_internal_lsa_conn(TALLOC_CTX *mem_ctx,
				       struct rpc_pipe_client **lsa_pipe,
				       struct policy_handle *lsa_hnd)
{
	NTSTATUS status;

	status = open_internal_lsa_pipe(mem_ctx, lsa_pipe);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	status = rpccli_lsa_open_policy((*lsa_pipe),
					mem_ctx,
					true,
					SEC_FLAG_MAXIMUM_ALLOWED,
					lsa_hnd);

	return status;
}

/*********************************************************************
 SAM specific functions.
*********************************************************************/

/* List all domain groups */
static NTSTATUS sam_enum_dom_groups(struct winbindd_domain *domain,
				    TALLOC_CTX *mem_ctx,
				    uint32_t *pnum_info,
				    struct acct_info **pinfo)
{
	struct rpc_pipe_client *samr_pipe;
	struct policy_handle dom_pol;
	struct acct_info *info = NULL;
	TALLOC_CTX *tmp_ctx;
	uint32_t start = 0;
	uint32_t num_info = 0;
	NTSTATUS status;

	DEBUG(3,("samr: query_user_list\n"));

	if (pnum_info) {
		*pnum_info = 0;
	}

	tmp_ctx = talloc_stackframe();
	if (tmp_ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	status = open_internal_samr_conn(tmp_ctx, domain, &samr_pipe, &dom_pol);
	if (!NT_STATUS_IS_OK(status)) {
		goto error;
	}

	do {
		struct samr_SamArray *sam_array = NULL;
		uint32_t count = 0;
		uint32_t g;

		/* start is updated by this call. */
		status = rpccli_samr_EnumDomainGroups(samr_pipe,
						      tmp_ctx,
						      &dom_pol,
						      &start,
						      &sam_array,
						      0xFFFF, /* buffer size? */
						      &count);
		if (!NT_STATUS_IS_OK(status)) {
			if (!NT_STATUS_EQUAL(status, STATUS_MORE_ENTRIES)) {
				DEBUG(2,("query_user_list: failed to enum domain groups: %s\n",
					 nt_errstr(status)));
				goto error;
			}
		}

		info = TALLOC_REALLOC_ARRAY(tmp_ctx,
					    info,
					    struct acct_info,
					    num_info + count);
		if (info == NULL) {
			status = NT_STATUS_NO_MEMORY;
			goto error;
		}

		for (g = 0; g < count; g++) {
			fstrcpy(info[num_info + g].acct_name,
				sam_array->entries[g].name.string);

			info[num_info + g].rid = sam_array->entries[g].idx;
		}

		num_info += count;
	} while (NT_STATUS_EQUAL(status, STATUS_MORE_ENTRIES));

	if (pnum_info) {
		*pnum_info = num_info;
	}

	if (pinfo) {
		*pinfo = talloc_move(mem_ctx, &info);
	}

error:
	TALLOC_FREE(tmp_ctx);
	return status;
}

/* Query display info for a domain */
static NTSTATUS sam_query_user_list(struct winbindd_domain *domain,
				    TALLOC_CTX *mem_ctx,
				    uint32_t *pnum_info,
				    struct wbint_userinfo **pinfo)
{
	struct rpc_pipe_client *samr_pipe = NULL;
	struct wbint_userinfo *info = NULL;
	struct policy_handle dom_pol;
	uint32_t num_info = 0;
	uint32_t loop_count = 0;
	uint32_t start_idx = 0;
	uint32_t i = 0;
	TALLOC_CTX *tmp_ctx;
	NTSTATUS status;

	DEBUG(3,("samr: query_user_list\n"));

	if (pnum_info) {
		*pnum_info = 0;
	}

	tmp_ctx = talloc_stackframe();
	if (tmp_ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	status = open_internal_samr_conn(tmp_ctx, domain, &samr_pipe, &dom_pol);
	if (!NT_STATUS_IS_OK(status)) {
		goto error;
	}

	do {
		uint32_t j;
		uint32_t num_dom_users;
		uint32_t max_entries, max_size;
		uint32_t total_size, returned_size;
		union samr_DispInfo disp_info;

		get_query_dispinfo_params(loop_count,
					  &max_entries,
					  &max_size);

		status = rpccli_samr_QueryDisplayInfo(samr_pipe,
						      tmp_ctx,
						      &dom_pol,
						      1, /* level */
						      start_idx,
						      max_entries,
						      max_size,
						      &total_size,
						      &returned_size,
						      &disp_info);
		if (!NT_STATUS_IS_OK(status)) {
			if (!NT_STATUS_EQUAL(status, STATUS_MORE_ENTRIES)) {
				goto error;
			}
		}

		/* increment required start query values */
		start_idx += disp_info.info1.count;
		loop_count++;
		num_dom_users = disp_info.info1.count;

		num_info += num_dom_users;

		info = TALLOC_REALLOC_ARRAY(tmp_ctx,
					    info,
					    struct wbint_userinfo,
					    num_info);
		if (info == NULL) {
			status = NT_STATUS_NO_MEMORY;
			goto error;
		}

		for (j = 0; j < num_dom_users; i++, j++) {
			uint32_t rid = disp_info.info1.entries[j].rid;
			struct samr_DispEntryGeneral *src;
			struct wbint_userinfo *dst;

			src = &(disp_info.info1.entries[j]);
			dst = &(info[i]);

			dst->acct_name = talloc_strdup(info,
						       src->account_name.string);
			if (dst->acct_name == NULL) {
				status = NT_STATUS_NO_MEMORY;
				goto error;
			}

			dst->full_name = talloc_strdup(info, src->full_name.string);
			if (dst->full_name == NULL) {
				status = NT_STATUS_NO_MEMORY;
				goto error;
			}

			dst->homedir = NULL;
			dst->shell = NULL;

			sid_compose(&dst->user_sid, &domain->sid, rid);

			/* For the moment we set the primary group for
			   every user to be the Domain Users group.
			   There are serious problems with determining
			   the actual primary group for large domains.
			   This should really be made into a 'winbind
			   force group' smb.conf parameter or
			   something like that. */
			sid_compose(&dst->group_sid, &domain->sid,
				    DOMAIN_RID_USERS);
		}
	} while (NT_STATUS_EQUAL(status, STATUS_MORE_ENTRIES));

	if (pnum_info) {
		*pnum_info = num_info;
	}

	if (pinfo) {
		*pinfo = talloc_move(mem_ctx, &info);
	}

error:
	TALLOC_FREE(tmp_ctx);
	return status;
}

/* Lookup user information from a rid or username. */
static NTSTATUS sam_query_user(struct winbindd_domain *domain,
			       TALLOC_CTX *mem_ctx,
			       const struct dom_sid *user_sid,
			       struct wbint_userinfo *user_info)
{
	struct rpc_pipe_client *samr_pipe;
	struct policy_handle dom_pol, user_pol;
	union samr_UserInfo *info = NULL;
	TALLOC_CTX *tmp_ctx;
	uint32_t user_rid;
	NTSTATUS status;

	DEBUG(3,("samr: query_user\n"));

	if (!sid_peek_check_rid(&domain->sid, user_sid, &user_rid)) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (user_info) {
		user_info->homedir = NULL;
		user_info->shell = NULL;
		user_info->primary_gid = (gid_t) -1;
	}

	tmp_ctx = talloc_stackframe();
	if (tmp_ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	status = open_internal_samr_conn(tmp_ctx, domain, &samr_pipe, &dom_pol);
	if (!NT_STATUS_IS_OK(status)) {
		goto error;
	}

	/* Get user handle */
	status = rpccli_samr_OpenUser(samr_pipe,
				      tmp_ctx,
				      &dom_pol,
				      SEC_FLAG_MAXIMUM_ALLOWED,
				      user_rid,
				      &user_pol);
	if (!NT_STATUS_IS_OK(status)) {
		goto error;
	}

	/* Get user info */
	status = rpccli_samr_QueryUserInfo(samr_pipe,
					   tmp_ctx,
					   &user_pol,
					   0x15,
					   &info);

	rpccli_samr_Close(samr_pipe, tmp_ctx, &user_pol);

	if (!NT_STATUS_IS_OK(status)) {
		goto error;
	}

	sid_compose(&user_info->user_sid, &domain->sid, user_rid);
	sid_compose(&user_info->group_sid, &domain->sid,
		    info->info21.primary_gid);

	if (user_info) {
		user_info->acct_name = talloc_strdup(mem_ctx,
						     info->info21.account_name.string);
		if (user_info->acct_name == NULL) {
			status = NT_STATUS_NO_MEMORY;
			goto error;
		}

		user_info->full_name = talloc_strdup(mem_ctx,
						     info->info21.full_name.string);
		if (user_info->acct_name == NULL) {
			status = NT_STATUS_NO_MEMORY;
			goto error;
		}

		user_info->homedir = NULL;
		user_info->shell = NULL;
		user_info->primary_gid = (gid_t)-1;
	}

	status = NT_STATUS_OK;
error:
	TALLOC_FREE(tmp_ctx);
	return status;
}

/* get a list of trusted domains - builtin domain */
static NTSTATUS sam_trusted_domains(struct winbindd_domain *domain,
				    TALLOC_CTX *mem_ctx,
				    struct netr_DomainTrustList *trusts)
{
	struct rpc_pipe_client *lsa_pipe;
	struct netr_DomainTrust *array = NULL;
	struct policy_handle lsa_policy;
	uint32_t enum_ctx = 0;
	uint32_t count = 0;
	TALLOC_CTX *tmp_ctx;
	NTSTATUS status;

	DEBUG(3,("samr: trusted domains\n"));

	tmp_ctx = talloc_stackframe();
	if (tmp_ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	status = open_internal_lsa_conn(tmp_ctx, &lsa_pipe, &lsa_policy);
	if (!NT_STATUS_IS_OK(status)) {
		goto error;
	}

	do {
		struct lsa_DomainList dom_list;
		uint32_t start_idx;
		uint32_t i;

		/*
		 * We don't run into deadlocks here, cause winbind_off() is
		 * called in the main function.
		 */
		status = rpccli_lsa_EnumTrustDom(lsa_pipe,
						 tmp_ctx,
						 &lsa_policy,
						 &enum_ctx,
						 &dom_list,
						 (uint32_t) -1);
		if (!NT_STATUS_IS_OK(status)) {
			if (!NT_STATUS_EQUAL(status, STATUS_MORE_ENTRIES)) {
				goto error;
			}
		}

		start_idx = trusts->count;
		count += dom_list.count;

		array = talloc_realloc(tmp_ctx,
				       array,
				       struct netr_DomainTrust,
				       count);
		if (array == NULL) {
			status = NT_STATUS_NO_MEMORY;
			goto error;
		}

		for (i = 0; i < dom_list.count; i++) {
			struct netr_DomainTrust *trust = &array[i];
			struct dom_sid *sid;

			ZERO_STRUCTP(trust);

			trust->netbios_name = talloc_move(array,
							  &dom_list.domains[i].name.string);
			trust->dns_name = NULL;

			sid = talloc(array, struct dom_sid);
			if (sid == NULL) {
				status = NT_STATUS_NO_MEMORY;
				goto error;
			}
			sid_copy(sid, dom_list.domains[i].sid);
			trust->sid = sid;
		}
	} while (NT_STATUS_EQUAL(status, STATUS_MORE_ENTRIES));

	if (trusts) {
		trusts->count = count;
		trusts->array = talloc_move(mem_ctx, &array);
	}

error:
	TALLOC_FREE(tmp_ctx);
	return status;
}

/* Lookup group membership given a rid.   */
static NTSTATUS sam_lookup_groupmem(struct winbindd_domain *domain,
				    TALLOC_CTX *mem_ctx,
				    const struct dom_sid *group_sid,
				    enum lsa_SidType type,
				    uint32_t *pnum_names,
				    struct dom_sid **psid_mem,
				    char ***pnames,
				    uint32_t **pname_types)
{
	struct rpc_pipe_client *samr_pipe;
	struct policy_handle dom_pol, group_pol;
	uint32_t samr_access = SEC_FLAG_MAXIMUM_ALLOWED;
	struct samr_RidTypeArray *rids = NULL;
	uint32_t group_rid;
	uint32_t *rid_mem = NULL;

	uint32_t num_names = 0;
	uint32_t total_names = 0;
	struct dom_sid *sid_mem = NULL;
	char **names = NULL;
	uint32_t *name_types = NULL;

	struct lsa_Strings tmp_names;
	struct samr_Ids tmp_types;

	uint32_t j, r;
	TALLOC_CTX *tmp_ctx;
	NTSTATUS status;

	DEBUG(3,("samr: lookup groupmem\n"));

	if (pnum_names) {
		pnum_names = 0;
	}

	if (!sid_peek_check_rid(&domain->sid, group_sid, &group_rid)) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	tmp_ctx = talloc_stackframe();
	if (tmp_ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	status = open_internal_samr_conn(tmp_ctx, domain, &samr_pipe, &dom_pol);
	if (!NT_STATUS_IS_OK(status)) {
		goto error;
	}

	status = rpccli_samr_OpenGroup(samr_pipe,
				       tmp_ctx,
				       &dom_pol,
				       samr_access,
				       group_rid,
				       &group_pol);
	if (!NT_STATUS_IS_OK(status)) {
		goto error;
	}

	/*
	 * Step #1: Get a list of user rids that are the members of the group.
	 */
	status = rpccli_samr_QueryGroupMember(samr_pipe,
					      tmp_ctx,
					      &group_pol,
					      &rids);

	rpccli_samr_Close(samr_pipe, tmp_ctx, &group_pol);

	if (!NT_STATUS_IS_OK(status)) {
		goto error;
	}

	if (rids == NULL || rids->count == 0) {
		pnum_names = 0;
		pnames = NULL;
		pname_types = NULL;
		psid_mem = NULL;

		status = NT_STATUS_OK;
		goto error;
	}

	num_names = rids->count;
	rid_mem = rids->rids;

	/*
	 * Step #2: Convert list of rids into list of usernames.
	 */
#define MAX_LOOKUP_RIDS 900

	if (num_names > 0) {
		names = TALLOC_ZERO_ARRAY(tmp_ctx, char *, num_names);
		name_types = TALLOC_ZERO_ARRAY(tmp_ctx, uint32_t, num_names);
		sid_mem = TALLOC_ZERO_ARRAY(tmp_ctx, struct dom_sid, num_names);
		if (names == NULL || name_types == NULL || sid_mem == NULL) {
			status = NT_STATUS_NO_MEMORY;
			goto error;
		}
	}

	for (j = 0; j < num_names; j++) {
		sid_compose(&sid_mem[j], &domain->sid, rid_mem[j]);
	}

	status = rpccli_samr_LookupRids(samr_pipe,
					tmp_ctx,
					&dom_pol,
					num_names,
					rid_mem,
					&tmp_names,
					&tmp_types);
	if (!NT_STATUS_IS_OK(status)) {
		if (!NT_STATUS_EQUAL(status, STATUS_SOME_UNMAPPED)) {
			goto error;
		}
	}

	/* Copy result into array.  The talloc system will take
	   care of freeing the temporary arrays later on. */
	if (tmp_names.count != tmp_types.count) {
		status = NT_STATUS_UNSUCCESSFUL;
		goto error;
	}

	for (r = 0; r < tmp_names.count; r++) {
		if (tmp_types.ids[r] == SID_NAME_UNKNOWN) {
			continue;
		}
		names[total_names] = fill_domain_username_talloc(names,
								 domain->name,
								 tmp_names.names[r].string,
								 true);
		name_types[total_names] = tmp_types.ids[r];
		total_names++;
	}

	if (pnum_names) {
		*pnum_names = total_names;
	}

	if (pnames) {
		*pnames = talloc_move(mem_ctx, &names);
	}

	if (pname_types) {
		*pname_types = talloc_move(mem_ctx, &name_types);
	}

	if (psid_mem) {
		*psid_mem = talloc_move(mem_ctx, &sid_mem);
	}

error:
	TALLOC_FREE(tmp_ctx);
	return status;
}

/*********************************************************************
 BUILTIN specific functions.
*********************************************************************/

/* List all domain groups */
static NTSTATUS builtin_enum_dom_groups(struct winbindd_domain *domain,
				TALLOC_CTX *mem_ctx,
				uint32 *num_entries,
				struct acct_info **info)
{
	/* BUILTIN doesn't have domain groups */
	*num_entries = 0;
	*info = NULL;
	return NT_STATUS_OK;
}

/* Query display info for a domain */
static NTSTATUS builtin_query_user_list(struct winbindd_domain *domain,
				TALLOC_CTX *mem_ctx,
				uint32 *num_entries,
				struct wbint_userinfo **info)
{
	/* We don't have users */
	*num_entries = 0;
	*info = NULL;
	return NT_STATUS_OK;
}

/* Lookup user information from a rid or username. */
static NTSTATUS builtin_query_user(struct winbindd_domain *domain,
				TALLOC_CTX *mem_ctx,
				const struct dom_sid *user_sid,
				struct wbint_userinfo *user_info)
{
	return NT_STATUS_NO_SUCH_USER;
}

/* get a list of trusted domains - builtin domain */
static NTSTATUS builtin_trusted_domains(struct winbindd_domain *domain,
					TALLOC_CTX *mem_ctx,
					struct netr_DomainTrustList *trusts)
{
	ZERO_STRUCTP(trusts);
	return NT_STATUS_OK;
}

/*********************************************************************
 COMMON functions.
*********************************************************************/

/* List all local groups (aliases) */
static NTSTATUS common_enum_local_groups(struct winbindd_domain *domain,
					 TALLOC_CTX *mem_ctx,
					 uint32_t *pnum_info,
					 struct acct_info **pinfo)
{
	struct rpc_pipe_client *samr_pipe;
	struct policy_handle dom_pol;
	struct acct_info *info = NULL;
	uint32_t num_info = 0;
	TALLOC_CTX *tmp_ctx;
	NTSTATUS status;

	DEBUG(3,("samr: enum local groups\n"));

	if (pnum_info) {
		*pnum_info = 0;
	}

	tmp_ctx = talloc_stackframe();
	if (tmp_ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	status = open_internal_samr_conn(tmp_ctx, domain, &samr_pipe, &dom_pol);
	if (!NT_STATUS_IS_OK(status)) {
		goto error;
	}

	do {
		struct samr_SamArray *sam_array = NULL;
		uint32_t count = 0;
		uint32_t start = num_info;
		uint32_t g;

		status = rpccli_samr_EnumDomainAliases(samr_pipe,
						       tmp_ctx,
						       &dom_pol,
						       &start,
						       &sam_array,
						       0xFFFF, /* buffer size? */
						       &count);
		if (!NT_STATUS_IS_OK(status)) {
			if (!NT_STATUS_EQUAL(status, STATUS_MORE_ENTRIES)) {
				goto error;
			}
		}

		info = TALLOC_REALLOC_ARRAY(tmp_ctx,
					    info,
					    struct acct_info,
					    num_info + count);
		if (info == NULL) {
			status = NT_STATUS_NO_MEMORY;
			goto error;
		}

		for (g = 0; g < count; g++) {
			fstrcpy(info[num_info + g].acct_name,
				sam_array->entries[g].name.string);
			info[num_info + g].rid = sam_array->entries[g].idx;
		}

		num_info += count;
	} while (NT_STATUS_EQUAL(status, STATUS_MORE_ENTRIES));

	if (pnum_info) {
		*pnum_info = num_info;
	}

	if (pinfo) {
		*pinfo = talloc_move(mem_ctx, &info);
	}

error:
	TALLOC_FREE(tmp_ctx);
	return status;
}

/* convert a single name to a sid in a domain */
static NTSTATUS common_name_to_sid(struct winbindd_domain *domain,
				   TALLOC_CTX *mem_ctx,
				   const char *domain_name,
				   const char *name,
				   uint32_t flags,
				   struct dom_sid *sid,
				   enum lsa_SidType *type)
{
	struct rpc_pipe_client *lsa_pipe;
	struct policy_handle lsa_policy;
	enum lsa_SidType *types = NULL;
	struct dom_sid *sids = NULL;
	char *full_name = NULL;
	char *mapped_name = NULL;
	TALLOC_CTX *tmp_ctx;
	NTSTATUS status;

	DEBUG(3,("samr: name to sid\n"));

	tmp_ctx = talloc_stackframe();
	if (tmp_ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	status = open_internal_lsa_conn(tmp_ctx, &lsa_pipe, &lsa_policy);
	if (!NT_STATUS_IS_OK(status)) {
		goto error;
	}

	if (name == NULL || name[0] == '\0') {
		full_name = talloc_asprintf(tmp_ctx, "%s", domain_name);
	} else if (domain_name == NULL || domain_name[0] == '\0') {
		full_name = talloc_asprintf(tmp_ctx, "%s", name);
	} else {
		full_name = talloc_asprintf(tmp_ctx, "%s\\%s", domain_name, name);
	}

	if (full_name == NULL) {
		status = NT_STATUS_NO_MEMORY;
	}

	status = normalize_name_unmap(tmp_ctx, full_name, &mapped_name);
	/* Reset the full_name pointer if we mapped anything */
	if (NT_STATUS_IS_OK(status) ||
	    NT_STATUS_EQUAL(status, NT_STATUS_FILE_RENAMED)) {
		full_name = mapped_name;
	}

	DEBUG(3,("name_to_sid: %s for domain %s\n",
		 full_name ? full_name : "", domain_name ));

	/*
	 * We don't run into deadlocks here, cause winbind_off() is
	 * called in the main function.
	 */
	status = rpccli_lsa_lookup_names(lsa_pipe,
					 tmp_ctx,
					 &lsa_policy,
					 1, /* num_names */
					 (const char **) &full_name,
					 NULL, /* domains */
					 1, /* level */
					 &sids,
					 &types);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(2,("name_to_sid: failed to lookup name: %s\n",
			nt_errstr(status)));
		goto error;
	}

	if (sid) {
		sid_copy(sid, &sids[0]);
	}
	if (type) {
		*type = types[0];
	}

error:
	TALLOC_FREE(tmp_ctx);
	return status;
}

/* convert a domain SID to a user or group name */
static NTSTATUS common_sid_to_name(struct winbindd_domain *domain,
				   TALLOC_CTX *mem_ctx,
				   const struct dom_sid *sid,
				   char **domain_name,
				   char **name,
				   enum lsa_SidType *type)
{
	struct rpc_pipe_client *lsa_pipe;
	struct policy_handle lsa_policy;
	char *mapped_name = NULL;
	char **domains = NULL;
	char **names = NULL;
	enum lsa_SidType *types = NULL;
	TALLOC_CTX *tmp_ctx;
	NTSTATUS map_status;
	NTSTATUS status;

	DEBUG(3,("samr: sid to name\n"));

	tmp_ctx = talloc_stackframe();
	if (tmp_ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	status = open_internal_lsa_conn(tmp_ctx, &lsa_pipe, &lsa_policy);
	if (!NT_STATUS_IS_OK(status)) {
		goto error;
	}

	/*
	 * We don't run into deadlocks here, cause winbind_off() is called in
	 * the main function.
	 */
	status = rpccli_lsa_lookup_sids(lsa_pipe,
					tmp_ctx,
					&lsa_policy,
					1, /* num_sids */
					sid,
					&domains,
					&names,
					&types);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(2,("sid_to_name: failed to lookup sids: %s\n",
			nt_errstr(status)));
		goto error;
	}

	if (type) {
		*type = (enum lsa_SidType) types[0];
	}

	if (name) {
		map_status = normalize_name_map(tmp_ctx,
						domain,
						*name,
						&mapped_name);
		if (NT_STATUS_IS_OK(map_status) ||
		    NT_STATUS_EQUAL(map_status, NT_STATUS_FILE_RENAMED)) {
			*name = talloc_strdup(mem_ctx, mapped_name);
			DEBUG(5,("returning mapped name -- %s\n", *name));
		} else {
			*name = talloc_strdup(mem_ctx, names[0]);
		}
		if (*name == NULL) {
			status = NT_STATUS_NO_MEMORY;
			goto error;
		}
	}

	if (domain_name) {
		*domain_name = talloc_strdup(mem_ctx, domains[0]);
		if (*domain_name == NULL) {
			status = NT_STATUS_NO_MEMORY;
			goto error;
		}
	}

error:
	TALLOC_FREE(tmp_ctx);
	return status;
}

static NTSTATUS common_rids_to_names(struct winbindd_domain *domain,
				     TALLOC_CTX *mem_ctx,
				     const struct dom_sid *sid,
				     uint32 *rids,
				     size_t num_rids,
				     char **pdomain_name,
				     char ***pnames,
				     enum lsa_SidType **ptypes)
{
	struct rpc_pipe_client *lsa_pipe;
	struct policy_handle lsa_policy;
	enum lsa_SidType *types = NULL;
	char *domain_name = NULL;
	char **domains = NULL;
	char **names = NULL;
	struct dom_sid *sids;
	size_t i;
	TALLOC_CTX *tmp_ctx;
	NTSTATUS status;

	DEBUG(3,("samr: rids to names for domain %s\n", domain->name));

	tmp_ctx = talloc_stackframe();
	if (tmp_ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	status = open_internal_lsa_conn(tmp_ctx, &lsa_pipe, &lsa_policy);
	if (!NT_STATUS_IS_OK(status)) {
		goto error;
	}

	if (num_rids) {
		sids = TALLOC_ARRAY(tmp_ctx, struct dom_sid, num_rids);
		if (sids == NULL) {
			status = NT_STATUS_NO_MEMORY;
			goto error;
		}
	} else {
		sids = NULL;
	}

	for (i = 0; i < num_rids; i++) {
		if (!sid_compose(&sids[i], sid, rids[i])) {
			status = NT_STATUS_INTERNAL_ERROR;
			goto error;
		}
	}

	/*
	 * We don't run into deadlocks here, cause winbind_off() is called in
	 * the main function.
	 */
	status = rpccli_lsa_lookup_sids(lsa_pipe,
					tmp_ctx,
					&lsa_policy,
					num_rids,
					sids,
					&domains,
					&names,
					&types);
	if (!NT_STATUS_IS_OK(status) &&
	    !NT_STATUS_EQUAL(status, STATUS_SOME_UNMAPPED)) {
		DEBUG(2,("rids_to_names: failed to lookup sids: %s\n",
			nt_errstr(status)));
		goto error;
	}

	for (i = 0; i < num_rids; i++) {
		char *mapped_name = NULL;
		NTSTATUS map_status;

		if (types[i] != SID_NAME_UNKNOWN) {
			map_status = normalize_name_map(tmp_ctx,
							domain,
							names[i],
							&mapped_name);
			if (NT_STATUS_IS_OK(map_status) ||
			    NT_STATUS_EQUAL(map_status, NT_STATUS_FILE_RENAMED)) {
				TALLOC_FREE(names[i]);
				names[i] = talloc_strdup(names, mapped_name);
				if (names[i] == NULL) {
					status = NT_STATUS_NO_MEMORY;
					goto error;
				}
			}

			domain_name = domains[i];
		}
	}

	if (pdomain_name) {
		*pdomain_name = talloc_strdup(mem_ctx, domain_name);
		if (*pdomain_name == NULL) {
			status = NT_STATUS_NO_MEMORY;
			goto error;
		}
	}

	if (ptypes) {
		*ptypes = talloc_move(mem_ctx, &types);
	}

	if (pnames) {
		*pnames = talloc_move(mem_ctx, &names);
	}

error:
	TALLOC_FREE(tmp_ctx);
	return status;
}

static NTSTATUS common_lockout_policy(struct winbindd_domain *domain,
				      TALLOC_CTX *mem_ctx,
				      struct samr_DomInfo12 *policy)
{
	/* TODO FIXME */
	return NT_STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS common_password_policy(struct winbindd_domain *domain,
				       TALLOC_CTX *mem_ctx,
				       struct samr_DomInfo1 *policy)
{
	/* TODO FIXME */
	return NT_STATUS_NOT_IMPLEMENTED;
}

/* Lookup groups a user is a member of.  I wish Unix had a call like this! */
static NTSTATUS common_lookup_usergroups(struct winbindd_domain *domain,
					 TALLOC_CTX *mem_ctx,
					 const struct dom_sid *user_sid,
					 uint32_t *num_groups,
					 struct dom_sid **user_gids)
{
	/* TODO FIXME */
	return NT_STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS common_lookup_useraliases(struct winbindd_domain *domain,
					  TALLOC_CTX *mem_ctx,
					  uint32_t num_sids,
					  const struct dom_sid *sids,
					  uint32_t *p_num_aliases,
					  uint32_t **rids)
{
	/* TODO FIXME */
	return NT_STATUS_NOT_IMPLEMENTED;
}

/* find the sequence number for a domain */
static NTSTATUS common_sequence_number(struct winbindd_domain *domain,
				       uint32_t *seq)
{
	/* TODO FIXME */
	return NT_STATUS_NOT_IMPLEMENTED;
}

#if 0
/* the rpc backend methods are exposed via this structure */
struct winbindd_methods builtin_passdb_methods = {
	.consistent            = false,

	.query_user_list       = builtin_query_user_list,
	.enum_dom_groups       = builtin_enum_dom_groups,
	.enum_local_groups     = common_enum_local_groups,
	.name_to_sid           = common_name_to_sid,
	.sid_to_name           = common_sid_to_name,
	.rids_to_names         = common_rids_to_names,
	.query_user            = builtin_query_user,
	.lookup_usergroups     = common_lookup_usergroups,
	.lookup_useraliases    = common_lookup_useraliases,
	.lookup_groupmem       = sam_lookup_groupmem,
	.sequence_number       = common_sequence_number,
	.lockout_policy        = common_lockout_policy,
	.password_policy       = common_password_policy,
	.trusted_domains       = builtin_trusted_domains
};

/* the rpc backend methods are exposed via this structure */
struct winbindd_methods sam_passdb_methods = {
	.consistent            = false,

	.query_user_list       = sam_query_user_list,
	.enum_dom_groups       = sam_enum_dom_groups,
	.enum_local_groups     = common_enum_local_groups,
	.name_to_sid           = common_name_to_sid,
	.sid_to_name           = common_sid_to_name,
	.rids_to_names         = common_rids_to_names,
	.query_user            = sam_query_user,
	.lookup_usergroups     = common_lookup_usergroups,
	.lookup_useraliases    = common_lookup_useraliases,
	.lookup_groupmem       = sam_lookup_groupmem,
	.sequence_number       = common_sequence_number,
	.lockout_policy        = common_lockout_policy,
	.password_policy       = common_password_policy,
	.trusted_domains       = sam_trusted_domains
};
#endif