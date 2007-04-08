/*
 Copyright (c) 2004-2006 NFG Net Facilities Group BV support@nfg.nl

 This program is free software; you can redistribute it and/or 
 modify it under the terms of the GNU General Public License 
 as published by the Free Software Foundation; either 
 version 2 of the License, or (at your option) any later 
 version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* 
 * This is the dbmail-user program
 * It makes adding users easier */

#include "dbmail.h"

extern char *configFile;

/* database login data */
extern db_param_t _db_params;

#define SHADOWFILE "/etc/shadow"
#define THIS_MODULE "user"

static char *getToken(char **str, const char *delims);
static char csalt[] = "........";
static char *bgetpwent(const char *filename, const char *name);
static char *cget_salt(void);

/* valid characters for passwd/username */
static const char ValidChars[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    "_.!@#$%^&*()-+=~[]{}<>:;\\/";

extern int yes_to_all = 0;
extern int no_to_all = 0;
extern int verbose = 0;
extern int quiet = 0; 		/* Don't be helpful. */
extern int reallyquiet = 0;	/* Don't print errors. */

int do_add(const char * const user,
           const char * const password, const char * const enctype,
           const u64_t maxmail, const u64_t clientid,
	   GList * alias_add,
	   GList * alias_del)
{
	u64_t useridnr;
	u64_t mailbox_idnr;
	int result;

	if (no_to_all) {
		qprintf("Pretending to add user %s with password type %s, %llu bytes mailbox limit and clientid %llu\n",
			user, enctype, maxmail, clientid);
		return 1;
	}

	TRACE(TRACE_DEBUG, "Adding user %s with password type %s,%llu "
		"bytes mailbox limit and clientid %llu... ", 
		user, enctype, maxmail, clientid);

	if ((result = auth_user_exists(user, &useridnr))) {
		qerrorf("Failed: check logs for details\n");
		return result;
	}

	if (auth_adduser(user, password, enctype, clientid, maxmail, &useridnr) < 0) {
		qerrorf("Failed: unable to create user\n");
		return -1;
	}

	TRACE(TRACE_DEBUG, "Ok, user added id [%llu]\n", useridnr);

	/* Add an INBOX for the user. */
	qprintf("Adding INBOX for new user... ");

	if (db_createmailbox("INBOX", useridnr, &mailbox_idnr) < 0) {
		qprintf("failed... removing user... ");
		if (auth_delete_user(user))
			qprintf("failed also.\n");
		else
			qprintf("done.\n");
		return -1;
	}
	qprintf("ok.\n");

	TRACE(TRACE_DEBUG, "Ok. INBOX created for user.\n");

	if(do_aliases(useridnr, alias_add, alias_del) < 0)
		result = -1;

	do_show(user);

	return result;
}

/* Change of username */
int do_username(const u64_t useridnr, const char * const newuser)
{
	int result = 0;

	assert(newuser);

	if (no_to_all) {
		qprintf("Pretending to change username of user id number [%llu] to [%s]\n",
			useridnr, newuser);
		return 1;
	}

	if ((result = auth_change_username(useridnr, newuser)))
		qerrorf("Error: could not change username.\n");

	return result;
}

/* Change of password */
int do_password(const u64_t useridnr,
                const char * const password, const char * const enctype)
{
	int result = 0;
	
	if (no_to_all) {
		qprintf("Pretending to change password for user id number [%llu]\n",
			useridnr);
		return 1;
	}

	if ((result = auth_change_password(useridnr, password, enctype)))
		qerrorf("Error: could not change password.\n");

	return result;
}

int mkpassword(const char * const user, const char * const passwd,
               const char * const passwdtype, const char * const passwdfile,
	       char ** password, char ** enctype)
{

	pwtype_t pwtype;
	int pwindex = 0;
	int result = 0;
	char *entry = NULL;
	char *md5str = NULL;
	char pw[50];

	/* These are the easy text names. */
	const char * const pwtypes[] = {
		"plaintext",	"plaintext-raw",	"crypt",	"crypt-raw",
		"md5", 		"md5-raw",		"md5sum",	"md5sum-raw", 
		"md5-hash",	"md5-hash-raw",		"md5-digest",	"md5-digest-raw",
		"md5-base64",	"md5-base64-raw",	"md5base64",	"md5base64-raw",
		"shadow", 	"", 	NULL
	};

	/* These must correspond to the easy text names. */
	const pwtype_t pwtypecodes[] = {
		PLAINTEXT, 	PLAINTEXT_RAW, 		CRYPT,		CRYPT_RAW,
		MD5_HASH, 	MD5_HASH_RAW,		MD5_DIGEST,	MD5_DIGEST_RAW,
		MD5_HASH,	MD5_HASH_RAW,		MD5_DIGEST,	MD5_DIGEST_RAW,
		MD5_BASE64,	MD5_BASE64_RAW,		MD5_BASE64,	MD5_BASE64_RAW,
		SHADOW,		PLAINTEXT,	PWTYPE_NULL
	};

	memset(pw, 0, 50);

	/* Only search if there's a string to compare. */
	if (passwdtype)
		/* Find a matching pwtype. */
		for (pwindex = 0; pwtypecodes[pwindex] != PWTYPE_NULL; pwindex++)
			if (strcasecmp(passwdtype, pwtypes[pwindex]) == 0)
				break;

	/* If no search took place, pwindex is 0, PLAINTEXT. */
	pwtype = pwtypecodes[pwindex];
	switch (pwtype) {
		case PLAINTEXT:
		case PLAINTEXT_RAW:
			null_strncpy(pw, passwd, 49);
			*enctype = "";
			break;
		case CRYPT:
			strcat(pw, null_crypt(passwd, cget_salt()));
			*enctype = "crypt";
			break;
		case CRYPT_RAW:
			null_strncpy(pw, passwd, 49);
			*enctype = "crypt";
			break;
		case MD5_HASH:
			sprintf(pw, "%s%s%s", "$1$", cget_salt(), "$");
			null_strncpy(pw, null_crypt(passwd, pw), 49);
			*enctype = "md5";
			break;
		case MD5_HASH_RAW:
			null_strncpy(pw, passwd, 49);
			*enctype = "md5";
			break;
		case MD5_DIGEST:
			md5str = dm_md5((unsigned char *)passwd);
			null_strncpy(pw, md5str, 49);
			*enctype = "md5sum";
			dm_free(md5str);
			break;
		case MD5_DIGEST_RAW:
			null_strncpy(pw, passwd, 49);
			*enctype = "md5sum";
			break;
		case MD5_BASE64:
			{
			md5str = dm_md5_base64((unsigned char *)passwd);
			null_strncpy(pw, md5str, 49);
			*enctype = "md5base64";
			dm_free(md5str);
			}
			break;
		case MD5_BASE64_RAW:
			null_strncpy(pw, passwd, 49);
			*enctype = "md5base64";
			break;
		case SHADOW:
			entry = bgetpwent(passwdfile, user);
			if (!entry) {
				qerrorf("Error: cannot read file [%s], "
					"please make sure that you have "
					"permission to read this file.\n",
					passwdfile);
				result = -1;
				break;
			}
                
			null_strncpy(pw, entry, 49);
			if (strcmp(pw, "") == 0) {
				qerrorf("Error: password for user [%s] not found in file [%s].\n",
				     user, passwdfile);
				result = -1;
				break;
			}

			/* Safe because we know pw is 50 long. */
			if (strncmp(pw, "$1$", 3) == 0) {
				*enctype = "md5";
			} else {
				*enctype = "crypt";
			}
			break;
		default:
			qerrorf("Error: password type not supported [%s].\n",
				passwdtype);
			result = -1;
			break;
	}

	/* Pass this out of the function. */
	*password = dm_strdup(pw);

	return result;
}

/* Change of client id. */
int do_clientid(u64_t useridnr, u64_t clientid)
{	
	int result = 0;

	if (no_to_all) {
		qprintf("Pretending to change client for user id number [%llu] to client id number [%llu]\n",
			useridnr, clientid);
		return 1;
	}

	if ((result = auth_change_clientid(useridnr, clientid)))
		qerrorf("Warning: could not change client id\n");

	return result;
}

/* Change of quota / max mail. */
int do_maxmail(u64_t useridnr, u64_t maxmail)
{
	int result = 0;

	if (no_to_all) {
		qprintf("Pretending to change mail quota for user id number [%llu] to [%llu] bytes\n",
			useridnr, maxmail);
		return 1;
	}

	if ((result = auth_change_mailboxsize(useridnr, maxmail)))
		qerrorf("Error: could not change max mail size.\n");

	return result;
}

int do_forwards(const char * const alias, const u64_t clientid,
                GList * fwds_add,
                GList * fwds_del)
{
	int result = 0;
	char *forward;
	GList *current_fwds, *matching_fwds, *matching_fwds_del;

	if (no_to_all) {
		qprintf("Pretending to remove forwards for alias [%s]\n",
			alias);
		if (fwds_del) {
			fwds_del = g_list_first(fwds_del);
			while (fwds_del) {
				qprintf("  [%s]\n", (char *)fwds_del->data);
				fwds_del = g_list_next(fwds_del);
			}
		}
		qprintf("Pretending to add forwards for alias [%s]\n",
			alias);
		if (fwds_add) {
			fwds_add = g_list_first(fwds_add);
			while (fwds_add) {
				qprintf("  [%s]\n", (char *)fwds_add->data);
				fwds_add = g_list_next(fwds_add);
			}
		}
		return 1;
	}

	current_fwds = auth_get_aliases_ext(alias);

	/* Delete aliases for the user. */
	if (fwds_del) {
		fwds_del = g_list_first(fwds_del);
		while (fwds_del) {
			forward = (char *)fwds_del->data;

			// Look for a wildcard character
			if (strchr(forward, '?') || strchr(forward, '*')) {
				qprintf("[%s] matches:\n", forward);

				matching_fwds = match_glob_list(forward, current_fwds);

				matching_fwds_del = g_list_first(matching_fwds);
				while (matching_fwds_del) {
					forward = (char *)matching_fwds_del->data;

					qprintf("  [%s]\n", forward);

					if (auth_removealias_ext(alias, forward) < 0) {
						qerrorf("Error: could not remove forward [%s] \n", forward);
						result = -1;
					}
					if (! g_list_next(matching_fwds_del))
						break;
					matching_fwds_del = g_list_next(matching_fwds_del);
				}
			// Nope, just a standard single forward removal
			} else {
				qprintf("[%s]\n", forward);

				if (auth_removealias_ext(alias, forward) < 0) {
					qerrorf("Error: could not remove forward [%s] \n",
					     forward);
					result = -1;
				}
			}

			if (! g_list_next(fwds_del))
				break;
			fwds_del = g_list_next(fwds_del);
		}
	}

	/* Add aliases for the user. */
	if (fwds_add) {
		fwds_add = g_list_first(fwds_add);
		while (fwds_add) {
			forward = (char *)fwds_add->data;
			qprintf("[%s]\n", forward);

			if (auth_addalias_ext(alias, forward, clientid) < 0) {
				qerrorf("Error: could not add forward [%s]\n",
				     alias);
				result = -1;
			}
			if (! g_list_next(fwds_add))
				break;
			fwds_add = g_list_next(fwds_add);
		}
	}

	qprintf("Done\n");

	return result;
}

int do_aliases(const u64_t useridnr,
               GList * alias_add,
               GList * alias_del)
{
	int result = 0;
	char *alias;
	u64_t clientid;
	GList *current_aliases, *matching_aliases, *matching_alias_del;

	if (no_to_all) {
		qprintf("Pretending to remove aliases for user id number [%llu]\n",
			useridnr);
		if (alias_del) {
			alias_del = g_list_first(alias_del);
			while (alias_del) {
				qprintf("  [%s]\n", (char *)alias_del->data);
				alias_del = g_list_next(alias_del);
			}
		}
		qprintf("Pretending to add aliases for user id number [%llu]\n",
			useridnr);
		if (alias_add) {
			alias_add = g_list_first(alias_add);
			while (alias_add) {
				qprintf("  [%s]\n", (char *)alias_add->data);
				alias_add = g_list_next(alias_add);
			}
		}
		return 1;
	}

	auth_getclientid(useridnr, &clientid);

	current_aliases = auth_get_user_aliases(useridnr);

	/* Delete aliases for the user. */
	if (alias_del) {
		alias_del = g_list_first(alias_del);
		while (alias_del) {
			alias = (char *)alias_del->data;

			// Look for a wildcard character
			if (strchr(alias, '?') || strchr(alias, '*')) {
				qprintf("[%s] matches:\n", alias);

				matching_aliases = match_glob_list(alias, current_aliases);

				matching_alias_del = g_list_first(matching_aliases);
				while (matching_alias_del) {
					alias = (char *)matching_alias_del->data;

					qprintf("  [%s]\n", alias);

					if (auth_removealias(useridnr, alias) < 0) {
						qerrorf("Error: could not remove alias [%s] \n", alias);
						result = -1;
					}
					if (! g_list_next(matching_alias_del))
						break;
					matching_alias_del = g_list_next(matching_alias_del);
				}
			// Nope, just a standard single alias removal
			} else {
				qprintf("[%s]\n", alias);

				if (auth_removealias(useridnr, alias) < 0) {
					qerrorf("Error: could not remove alias [%s] \n", alias);
					result = -1;
				}
			}

			if (! g_list_next(alias_del))
				break;
			alias_del = g_list_next(alias_del);
		}
	}

	/* Add aliases for the user. */
	if (alias_add) {
		alias_add = g_list_first(alias_add);
		while (alias_add) {
			alias = (char *)alias_add->data;
			qprintf("[%s]\n", alias);

			if (strchr(alias, '?') || strchr(alias, '*')) {
				// Has wildcard!
			}

			if (auth_addalias (useridnr, alias, clientid) < 0) {
				qerrorf("Error: could not add alias [%s]\n", alias);
				result = -1;
			}
			if (! g_list_next(alias_add))
				break;
			alias_add = g_list_next(alias_add);
		}
	}

	qprintf("Done\n");

	return result;
}


int do_delete(const u64_t useridnr, const char * const name)
{
	int result;
	GList *aliases = NULL;

	if (no_to_all) {
		qprintf("Pretending to delete alias [%s] for user id number [%llu]\n",
			name, useridnr);
		return 1;
	}

	qprintf("Deleting aliases for user [%s]...\n", name);
	aliases = auth_get_user_aliases(useridnr);
	do_aliases(useridnr, NULL, aliases);

	qprintf("Deleting user [%s]...\n", name);
	result = auth_delete_user(name);

	if (result < 0) {
		qprintf("Failed. Please check the log\n");
		return -1;
	}

	qprintf("Done\n");
	return 0;
}

int do_show(const char * const name)
{
	u64_t useridnr, cid, quotum, quotumused;
	GList *users = NULL;
	GList *userlist = NULL;
	char *username;
	int result;
	struct dm_list uids;
	struct dm_list fwds;
	GList *userids = NULL;
	GList *forwards = NULL;

	if (!name) {
		/* show all users */
		users = auth_get_known_users();
		if (g_list_length(users) > 0) {
			users = g_list_first(users);
			while (users) {
				do_show(users->data);
				if (! g_list_next(users))
					break;
				users = g_list_next(users);
			}
			g_list_foreach(users,(GFunc)g_free,NULL);
		}
		g_list_free(users);

	} else {
		if (auth_user_exists(name, &useridnr) == -1) {
			qerrorf("Error while verifying user [%s].\n", name);
			return -1;
		}

		if (useridnr == 0) {
			/* not a user, search aliases */
			dm_list_init(&fwds);
			dm_list_init(&uids);
			result = auth_check_user_ext(name,&uids,&fwds,0);
			
			if (!result) {
				qerrorf("Nothing found searching for [%s].\n", name);
				return -1;
			}
		
			if (dm_list_getstart(&uids))
				userids = g_list_copy_list(userids,dm_list_getstart(&uids));
			if (dm_list_getstart(&fwds))
				forwards = g_list_copy_list(forwards,dm_list_getstart(&fwds));
			
			forwards = g_list_first(forwards);
			if (forwards) {
				while(forwards) {
					qerrorf("forward [%s] to [%s]\n", name, (char *)forwards->data);
					if (! g_list_next(forwards))
						break;
					forwards = g_list_next(forwards);
				}
				g_list_foreach(forwards,(GFunc)g_free, NULL);
				g_list_free(forwards);
			}
			
			userids = g_list_first(userids);
			if (userids) {
				while (userids) {
					username = auth_get_userid(*(u64_t *)userids->data);
					qerrorf("deliver [%s] to [%s]\n-------\n", name, username);
					do_show(username);
					g_free(username);
					if (! g_list_next(userids))
						break;
					userids = g_list_next(userids);
				}
				g_list_free(userids);
			}
			return 0;
		}

		auth_getclientid(useridnr, &cid);
		auth_getmaxmailsize(useridnr, &quotum);
		db_get_quotum_used(useridnr, &quotumused);

		GList *out = NULL;
		GString *s = g_string_new("");
		
		username = auth_get_userid(useridnr);
		out = g_list_append_printf(out,"%s", username);
		g_free(username);
		
		out = g_list_append_printf(out,"x");
		out = g_list_append_printf(out,"%llu", useridnr);
		out = g_list_append_printf(out,"%llu", cid);
		out = g_list_append_printf(out,"%.02f", 
				(double) quotum / (1024.0 * 1024.0));
		out = g_list_append_printf(out,"%.02f", 
				(double) quotumused / (1024.0 * 1024.0));
		userlist = auth_get_user_aliases(useridnr);

		if (g_list_length(userlist)) {
			userlist = g_list_first(userlist);
			s = g_list_join(userlist,",");
			g_list_append_printf(out,"%s", s->str);
			g_list_foreach(userlist,(GFunc)g_free, NULL);
		} else {
			g_list_append_printf(out,"");
		}
		g_list_free(userlist);
		s = g_list_join(out,":");
		qprintf("%s\n", s->str);
		g_string_free(s,TRUE);
	}

	return 0;
}


/*
 * empties the mailbox associated with user 'name'
 */
int do_empty(u64_t useridnr)
{
	int result;

	if (no_to_all) {
		u64_t *children;
		u64_t owner_idnr;
		unsigned nchildren, i;
		char mailbox[IMAP_MAX_MAILBOX_NAMELEN];

		qprintf("You've requested to delete all mailboxes owned by user number [%llu]:\n", useridnr);

		db_findmailbox_by_regex(useridnr, "*", &children, &nchildren, 0);
		for (i = 0; i < nchildren; i++) {
			/* Given a list of mailbox id numbers, check if the
			 * user actually owns the mailbox (because that means
			 * it is on the hit list for deletion) and then look up
			 * and print out the name of the mailbox. */
			db_get_mailbox_owner(children[i], &owner_idnr);
			if (owner_idnr == useridnr) {
				db_getmailboxname(children[i], useridnr, mailbox);			
				qprintf("%s\n", mailbox);
			}
		}

		qprintf("please run again with -y to actually perform this action.\n");
		return 1;
	}
//	if (yes_to_all) { // TODO: Require -y before taking such drastic action.
		qprintf("Emptying mailbox... ");
		fflush(stdout);
        
		result = db_empty_mailbox(useridnr);
		if (result != 0)
			qerrorf("Error. Please check the log.\n");
		else
			qprintf("Ok.\n");
//	}

	return result;
}


/*eddy
  This two function was base from "cpu" by Blake Matheny <matheny@dbaseiv.net>
  bgetpwent : get hash password from /etc/shadow
  cget_salt : generate salt value for crypt
*/
char *bgetpwent(const char *filename, const char *name)
{
	FILE *passfile = NULL;
	char pass_char[512];
	int pass_size = 511;
	char *pw = NULL;
	char *user = NULL;

	if ((passfile = fopen(filename, "r")) == NULL)
		return NULL;

	while (fgets(pass_char, pass_size, passfile) != NULL) {
		char *m = pass_char;
		int num_tok = 0;
		char *toks;

		while (m != NULL && *m != 0) {
			toks = getToken(&m, ":");
			if (num_tok == 0)
				user = toks;
			else if (num_tok == 1)
				/*result->pw_passwd = toks; */
				pw = toks;
			else
				break;
			num_tok++;
		}
		if (strcmp(user, name) == 0)
			return pw;

	}
	return "";
}

char *cget_salt()
{
	unsigned long seed[2];
	const char *const seedchars =
	    "./0123456789ABCDEFGHIJKLMNOPQRST"
	    "UVWXYZabcdefghijklmnopqrstuvwxyz";
	int i;

	seed[0] = time(NULL);
	seed[1] = getpid() ^ (seed[0] >> 14 & 0x30000);
	for (i = 0; i < 8; i++)
		csalt[i] = seedchars[(seed[i / 5] >> (i % 5) * 6) & 0x3f];

	return csalt;
}


/*
  This function was base on function of "cpu"
        by Blake Matheny <matheny@dbaseiv.net>
  getToken : break down username and password from a file
*/
char *getToken(char **str, const char *delims)
{
	char *token;

	if (*str == NULL) {
		/* No more tokens */
		return NULL;
	}

	token = *str;
	while (**str != '\0') {
		if (strchr(delims, **str) != NULL) {
			**str = '\0';
			(*str)++;
			return token;
		}
		(*str)++;
	}

	/* There is no other token */
	*str = NULL;
	return token;
}

u64_t strtomaxmail(const char * const str)
{
	u64_t maxmail;
	char *endptr = NULL;

	maxmail = strtoull(str, &endptr, 10);
	switch (*endptr) {
	case 'g':
	case 'G':
		maxmail *= (1024 * 1024 * 1024);
		break;

	case 'm':
	case 'M':
		maxmail *= (1024 * 1024);
		break;

	case 'k':
	case 'K':
		maxmail *= 1024;
		break;
	}

	return maxmail;
}

