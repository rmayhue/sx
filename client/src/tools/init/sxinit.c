/*
 *  Copyright (C) 2012-2014 Skylable Ltd. <info-copyright@skylable.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *  Special exception for linking this software with OpenSSL:
 *
 *  In addition, as a special exception, Skylable Ltd. gives permission to
 *  link the code of this program with the OpenSSL library and distribute
 *  linked combinations including the two. You must obey the GNU General
 *  Public License in all respects for all of the code used other than
 *  OpenSSL. You may extend this exception to your version of the program,
 *  but you are not obligated to do so. If you do not wish to do so, delete
 *  this exception statement from your version.
 */

#include "default.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/mman.h>

#include "sx.h"
#include "cmdline.h"
#include "version.h"
#include "libsx/src/misc.h"
#include "libsx/src/clustcfg.h"
#include "libsx/src/cluster.h"
#include "bcrumbs.h"

static sxc_client_t *sx = NULL;

static void sighandler(int signal) {
    struct termios tcur;
    if(sx)
	sxc_shutdown(sx, signal);
    /* work around for ctrl+c during sxc_pass2token() */
    tcgetattr(0, &tcur);
    tcur.c_lflag |= ECHO;
    tcsetattr(0, TCSANOW, &tcur);

    fprintf(stderr, "Process interrupted\n");
    exit(1);
}

/* List all clusters with profile names that are configured in configuration directory */
static int list_clusters(void) {
    const char *confdir = NULL;
    DIR *clusters_dir = NULL, *profiles_dir = NULL;
    struct dirent *cluster_dirent = NULL, *profile_dirent;

    confdir = sxc_get_confdir(sx);
    if(!confdir){
        fprintf(stderr, "ERROR: Could not locate configuration directory\n");
        return 1;
    }

    clusters_dir = opendir(confdir);
    if(!clusters_dir) {
	if(errno == ENOENT)
	    fprintf(stderr, "No profiles configured\n");
	else
	    fprintf(stderr, "ERROR: Could not open %s directory: %s\n", confdir, strerror(errno));
        return 1;
    }

    while((cluster_dirent = readdir(clusters_dir)) != NULL) {
        char *auth_dir_name = NULL;
        int auth_dir_len = 0;

        if(cluster_dirent->d_name[0] == '.') continue; /* Omit files and directories starting with . */

        auth_dir_len = strlen(confdir) + strlen(cluster_dirent->d_name) + strlen("/auth") + 2;
        auth_dir_name = malloc(auth_dir_len);
        if(!auth_dir_name) {
            fprintf(stderr, "ERROR: Could not allocate memory for auth directory\n");
            break;
        }
        snprintf(auth_dir_name, auth_dir_len, "%s/%s/auth", confdir, cluster_dirent->d_name);

        if(access(auth_dir_name, F_OK)) {
            free(auth_dir_name);
            continue;
        }

        profiles_dir = opendir(auth_dir_name);
        if(profiles_dir) {
            while((profile_dirent = readdir(profiles_dir)) != NULL) {
                if(profile_dirent->d_name[0] != '.') {
                    char *aliases = NULL;
                    int left_len = strlen("sx://") + strlen(profile_dirent->d_name) + strlen(cluster_dirent->d_name) + 2;
                    /* Left is prepared separately because we want to justify ouptut */
                    char *left = malloc(left_len);
                    if(!left) {
                        fprintf(stderr, "ERROR: Could not allocate memory\n");
                        break;
                    }
		    if(!strcmp(profile_dirent->d_name, "default"))
			snprintf(left, left_len, "sx://%s", cluster_dirent->d_name);
		    else
			snprintf(left, left_len, "sx://%s@%s", profile_dirent->d_name, cluster_dirent->d_name);
                    if(sxc_get_aliases(sx, profile_dirent->d_name, cluster_dirent->d_name, &aliases)) {
                        free(left);
                        fprintf(stderr, "ERROR: %s\n", sxc_geterrmsg(sx)); /* Error message should already be set */
                        break;
                    }
                    if(aliases)
                        printf("%-40s %s\n", left, aliases);
                    else
                        printf("%-40s %s\n", left, "-");
                    free(left);
                    free(aliases);
                }
            }

            closedir(profiles_dir);
        }
        free(auth_dir_name);
    }

    closedir(clusters_dir);
    return 0;
}

static int del_profile(sxc_uri_t *u) {
    int ret = -1;
    const char *config_dir = sxc_get_confdir(sx);
    unsigned int profdir_len, confdir_len, fname_len;
    const char *home_dir = NULL;
    char *fname;
    const char *profile;

    if(!u || !u->host) {
        sxi_seterr(sx, SXE_EARG, "Cannot locate config directory: Invalid argument");
        return ret;
    }

    confdir_len = strlen(u->host);
    if(memchr(u->host, '/', confdir_len)) {
        sxi_seterr(sx, SXE_EARG, "Cannot locate config directory: Invalid argument");
        return ret;
    }

    if(!config_dir) {
        home_dir = sxi_getenv("HOME");
        if(!home_dir) {
            struct passwd *pwd = getpwuid(geteuid());
            if(pwd)
                home_dir = pwd->pw_dir;
        }
        if(!home_dir) {
            sxi_seterr(sx, SXE_EARG, "Cannot locate config directory: Cannot determine home directory");
            return ret;
        }
        confdir_len += strlen(home_dir) + 2 + lenof(".sx");
    } else
        confdir_len += strlen(config_dir) + 1;

    if(!u->profile || !u->profile[0])
        profile = "default";
    else
        profile = u->profile;

    profdir_len = confdir_len + strlen("/auth/");
    fname_len = strlen(profile) + profdir_len + 1;
    fname = malloc(fname_len);
    if(!fname) {
        sxi_seterr(sx, SXE_EMEM, "Cannot locate config directory: Out of memory");
        goto rm_profile_err;
    }

    if(config_dir)
        snprintf(fname, fname_len, "%s/%s/auth/%s", config_dir, u->host, profile);
    else
        snprintf(fname, fname_len, "%s/.sx/%s/auth/%s", home_dir, u->host, profile);

    if(access(fname, F_OK)) {
        sxi_seterr(sx, SXE_ECFG, "Cannot locate profile 'sx://%s@%s/'", profile, u->host);
        goto rm_profile_err;
    }

    /* Remove profile key file */
    if(unlink(fname)) {
        sxi_seterr(sx, SXE_ECFG, "Cannot remove profile '%s': %s", profile, strerror(errno));
        goto rm_profile_err;
    }

    /* Clean all aliases combined with given profile */
    if(sxc_del_aliases(sx, profile, u->host)) {
        SXDEBUG("Failed to delete aliases for profile '%s': %s", profile, sxc_geterrmsg(sx));
        goto rm_profile_err;
    }

    fname[profdir_len] = '\0';
    /* Try to remove directory with all profiles, if succeeded remove whole cluster configuration since no profile is configured then */
    if(!rmdir(fname)) {
        /* Remove all subdirectories */
        fname[confdir_len] = '\0';
        if(sxi_rmdirs(fname)) {
            sxi_seterr(sx, SXE_ECFG, "Cannot remove cluster configuration directory: %s", fname);
            goto rm_profile_err;
        }
    }

    ret = 0;
rm_profile_err:
    free(fname);
    return ret;
}

/* Check if alias exists and if so, compare its uri with given by user */
static int check_alias(const char *alias, const sxc_uri_t *u) {
    sxc_uri_t *tmp = NULL;

    if(!alias || !u) {
        fprintf(stderr, "ERROR: Invalid argument\n");
        return 1;
    }

    if(strncmp(alias, SXC_ALIAS_PREFIX, lenof(SXC_ALIAS_PREFIX))) {
        fprintf(stderr, "ERROR: Bad alias name: it must start with %s\n", SXC_ALIAS_PREFIX);
        return 1;
    }

    if(strlen(alias) <= lenof(SXC_ALIAS_PREFIX)) {
        fprintf(stderr, "ERROR: Bad alias name: Alias name is too short\n");
        return 1;
    }

    if(!(tmp = sxc_parse_uri(sx, alias))) {
        if(sxc_geterrnum(sx) != SXE_ECFG) {
            fprintf(stderr, "ERROR: %s\n", sxc_geterrmsg(sx));
            return 1;
        }
        sxc_clearerr(sx);
        return 0;
    }

    /* Check host part of uri */
    if(strcmp(tmp->host, u->host)) {
        fprintf(stderr, "ERROR: Alias '%s' is already used\n", alias);
        sxc_free_uri(tmp);
        return 1;
    }

    if(!u->profile && !tmp->profile) { /* No profile defined, same uri */
        sxc_free_uri(tmp);
        return 0;
    }

    if(u->profile && tmp->profile) { /* Both uris have profile defined, compare them */
        if(strcmp(u->profile, tmp->profile)) {
            fprintf(stderr, "ERROR: Alias '%s' is already used\n", alias);
            sxc_free_uri(tmp);
            return 1;
        }
    } else { /* Different profiles for same host */
        fprintf(stderr, "ERROR: Alias '%s' is already used\n", alias);
        sxc_free_uri(tmp);
        return 1;
    }

    sxc_free_uri(tmp);
    return 0;
}

static int yesno(const char *prompt, int def)
{
    char c;
    while(1) {
	if(def)
	    printf("%s [Y/n] ", prompt);
	else
	    printf("%s [y/N] ", prompt);
	fflush(stdout);
	c = sxi_read_one_char();
	if(c == 'y' || c == 'Y')
	    return 1;
	if(c == 'n' || c == 'N')
	    return 0;
	if(c == '\n' || c == EOF)
	    return def;
    }
    return 0;
}

/* Fetch basic cluster information like security flag or UUID by doing a not authorised query with fake key */
static int fetch_cluster_noauth_info(sxc_cluster_t *cluster, const sxc_uri_t *u) {
    char tok_buf[AUTHTOK_ASCII_LEN+1];
    sxi_strlcpy(tok_buf, "wFPs+e1B3wMRud8TzGw7YHjS08LWGuoIdfALMZTPLMVFKYM41rVlDwAA", sizeof(tok_buf));
    if(sxc_cluster_add_access(cluster, u->profile, tok_buf) || sxc_cluster_set_access(cluster, u->profile)) {
        fprintf(stderr, "ERROR: Failed to set profile authentication: %s\n", sxc_geterrmsg(sx));
        return 1;
    }
    if(sxc_cluster_fetchnodes(cluster) && sxc_geterrnum(sx) != SXE_EAUTH) {
        fprintf(stderr, "ERROR: %s\n", sxc_geterrmsg(sx));
        return 1;
    }
    return 0;
}

struct init_conf {
    sxc_uri_t *uri;
    unsigned int port;
    char *token;
    char *hostlist;
    int ssl;
    const char *certhash;
    const char *alias;
};

static char *parse_config_link(const char *link, struct init_conf *c) {
    char *q, *dlink = NULL;
    int ret = 1;
    const char *ssl = NULL, *port = NULL;

    if(!link || !c) {
        fprintf(stderr, "ERROR: Invalid argument\n");
        return NULL;
    }

    if(!strncmp(link, "sx://", lenof("sx://"))) {
        dlink = strdup(link);
        if(!dlink) {
            fprintf(stderr, "ERROR: Failed to parse configuration link: Out of memory\n");
            goto parse_config_link_err;
        }
    } else {
        int len;

        dlink = calloc(1, 1024);
        if(!dlink) {
            fprintf(stderr, "ERROR: Failed to parse configuration link: Out of memory\n");
            goto parse_config_link_err;
        }

        if(strcmp(link, "-")) {
            FILE *f = fopen(link, "r");
            if(!f) {
                fprintf(stderr, "ERROR: Failed to open configuration link: Failed to open file %s\n", link);
                goto parse_config_link_err;
            }
            link = fgets(dlink, 1024, f);
            fclose(f);
        } else {
            printf("Please enter the configuration link: ");
            link = fgets(dlink, 1024, stdin);
        }

        if(!link) {
            fprintf(stderr, "ERROR: Failed to parse configuration link: Failed to read file\n");
            goto parse_config_link_err;
        }

        len = strlen(dlink);
        if(len && dlink[len - 1] == '\n')
            dlink[len - 1] = '\0';
    }

    q = strchr(dlink, '?');
    if(!q) {
        fprintf(stderr, "ERROR: Failed to parse configuration link: Missing token parameter\n");
        goto parse_config_link_err;
    }
    *q++ = '\0';

    c->uri = sxc_parse_uri(sx, dlink);
    if(!c->uri) {
        fprintf(stderr, "ERROR: Failed to parse configuration link: Invalid configuration URI\n");
        goto parse_config_link_err;
    }

    /* Start parsing key-value pairs */
    while(q) {
        char *key = q, *value = strchr(q, '=');
        if(!value) {
            fprintf(stderr, "ERROR: Failed to parse configuration link: Invalid configuration URI\n");
            goto parse_config_link_err;
        }
        *value++ = '\0';
        q = strchr(value, '&');
        if(q)
            *q++ = '\0';

        if(!strcmp(key, "token")) {
            c->token = value;
        } else if(!strcmp(key, "ip")) {
            c->hostlist = strdup(value);
            if(!c->hostlist) {
                fprintf(stderr, "ERROR: Failed to parse configuration link: Out of memory\n");
                goto parse_config_link_err;
            }
        } else if(!strcmp(key, "ssl")) {
            ssl = value;
        } else if(!strcmp(key, "port")) {
            port = value;
        }
    }

    if(!c->token) {
        fprintf(stderr, "ERROR: Failed to parse configuration link: Missing token\n");
        goto parse_config_link_err;
    }

    if(ssl && ((!strcmp(ssl, "n") && c->certhash) || (strcmp(ssl, "y") && strcmp(ssl, "n")))) {
        fprintf(stderr, "ERROR: Failed to parse configuration link: Invalid configuration URI\n");
        goto parse_config_link_err;
    }

    if(ssl && *ssl == 'n')
        c->ssl = 0;

    if(port) {
        char *enumb;
        long p = strtol(port, &enumb, 10);
        if(*enumb || p < 0) {
            fprintf(stderr, "ERROR: Failed to parse configuration link: Invalid port number\n");
            goto parse_config_link_err;
        }
        c->port = p;
    }

    ret = 0;
parse_config_link_err:
    if(ret) {
        free(dlink);
        return NULL;
    }
    return dlink;
}

int main(int argc, char **argv) {
    struct gengetopt_args_info args;
    sxc_cluster_t *cluster = NULL;
    sxc_logger_t log;
    int ret = 1;
    struct init_conf c;
    char *link = NULL;

    if(cmdline_parser(argc, argv, &args))
	return 1;

    if(args.version_given) {
	printf("%s %s\n", CMDLINE_PARSER_PACKAGE, SRC_VERSION);
	cmdline_parser_free(&args);
	return 0;
    }

    memset(&c, 0, sizeof(c));
    c.ssl = 1; /* Use SSL by default */

    /* Check if sx://profile@cluster/ or --list, or --config-link option is given but not together */
    if((args.inputs_num != 1 && !args.list_given && !args.config_link_given)
        || (args.inputs_num != 0 && (args.list_given || args.config_link_given)) /* --list and --config-link does not take inputs */
        || (args.pass_file_given && (args.config_link_given || args.auth_file_given)) /* --pass-file cannot be used with --config-link or --auth-file */
        || (args.config_link_given && (args.list_given || args.delete_given || args.info_given || args.auth_file_given))) {
	cmdline_parser_print_help();
	printf("\n");
	fprintf(stderr, "ERROR: Wrong number of arguments\n");
	goto init_err;
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    if(!(sx = sxc_init(SRC_VERSION, sxc_default_logger(&log, argv[0]), sxc_input_fn, NULL))) {
        fprintf(stderr, "ERROR: Failed to initialize SX\n");
	goto init_err;
    }

    if(args.config_dir_given && sxc_set_confdir(sx, args.config_dir_arg)) {
        fprintf(stderr, "ERROR: Could not set configuration directory %s: %s\n", args.config_dir_arg, sxc_geterrmsg(sx));
        goto init_err;
    }
    sxc_set_debug(sx, args.debug_flag);

    if(args.list_given)
    {
        ret = list_clusters();
        goto init_err;
    }

    if(args.config_link_given) {
        link = parse_config_link(args.config_link_arg, &c);
        if(!link) {
            ret = 1;
            goto init_err;
        }
    } else {
        c.uri = sxc_parse_uri(sx, args.inputs[0]);
        if(!c.uri) {
	    fprintf(stderr, "ERROR: Invalid SX URI %s\n", args.inputs[0]);
            goto init_err;
        }

        if(args.host_list_given) {
            c.hostlist = strdup(args.host_list_arg);
            if(!c.hostlist) {
                fprintf(stderr, "ERROR: Out of memory\n");
                goto init_err;
            }
        }

        if(args.no_ssl_given)
            c.ssl = 0;

        if(args.port_given) {
            if(args.port_arg <= 0) {
                fprintf(stderr, "ERROR: Invalid port given: %d\n", args.port_arg);
                goto init_err;
            }
            c.port = args.port_arg;
        }
    }

    if(args.alias_given) {
        c.alias = args.alias_arg;
        if(check_alias(c.alias, c.uri))
            goto init_err;
    }

    if(args.delete_given) {
        ret = del_profile(c.uri);
        if(ret)
            fprintf(stderr, "ERROR: %s\n", sxc_geterrmsg(sx));

        goto init_err;
    }

    if(!args.force_reinit_flag)
	cluster = sxc_cluster_load(sx, args.config_dir_arg, c.uri->host);

    if(args.info_given) {
        if(!cluster) {
            fprintf(stderr, "ERROR: Failed to load cluster: %s\n", sxc_geterrmsg(sx));
            goto init_err;
        }

        /* Print cluster information and exit */
        ret = sxc_cluster_info(cluster, c.uri);
        goto init_err;
    }

    if(!cluster) /* Either force-reinit or load failed */
	cluster = sxc_cluster_new(sx);

    if(!cluster) {
	fprintf(stderr, "ERROR: Cannot initialize new cluster: %s\n", sxc_geterrmsg(sx));
	goto init_err;
    }

    if(sxc_cluster_set_sslname(cluster, c.uri->host)) {
        fprintf(stderr, "ERROR: Cannot initialize new cluster: %s\n", sxc_geterrmsg(sx));
        goto init_err;
    }

    if(c.hostlist) {
	/* DNS-less cluster */
	char *this_host = c.hostlist, *next_host;

	if(sxc_cluster_set_dnsname(cluster, NULL)) {
	    fprintf(stderr, "ERROR: Cannot set cluster DNS-less flag: %s\n", sxc_geterrmsg(sx));
	    goto init_err;
	}

	sxc_cluster_reset_hosts(cluster);
	do {
	    next_host = strchr(this_host, ',');
	    if(next_host) {
		*next_host = '\0';
		next_host++;
	    }
	    if(sxc_cluster_add_host(cluster, this_host)) {
		fprintf(stderr, "ERROR: Cannot add %s to cluster nodes: %s\n", this_host, sxc_geterrmsg(sx));
		goto init_err;
	    }
	    this_host = next_host;
	} while(this_host);
    } else {
	/* DNS based cluster */
	if(sxc_cluster_set_dnsname(cluster, c.uri->host)) {
	    fprintf(stderr, "ERROR: Cannot set cluster DNS name to %s: %s\n", c.uri->host, sxc_geterrmsg(sx));
	    goto init_err;
	}
    }

    if(c.port && sxc_cluster_set_httpport(cluster, c.port)) {
	fprintf(stderr, "ERROR: Failed to configure cluster communication port\n");
	    goto init_err;
    }

    if(!c.ssl) {
	/* NON-SSL cluster */
	if(sxc_cluster_set_cafile(cluster, NULL)) {
	    fprintf(stderr, "ERROR: Failed to configure cluster security\n");
	    goto init_err;
	}

	if(!args.batch_mode_flag) {
	    /* do a bogus query with a fake key to get the remote security flag */
            if(fetch_cluster_noauth_info(cluster, c.uri))
                goto init_err; /* Error message has already been printed */

	    if(sxi_conns_internally_secure(sxi_cluster_get_conns(cluster)) == 1) {
		printf("*** WARNING ***: The cluster reports secure internal communication, however you're attempting to connect with the SSL disabled.\n");
		if(!yesno("Do you want to continue?", 0)) {
		    fprintf(stderr, "Aborted\n");
		    goto init_err;
		}
	    }
	}
    } else {
	/* SSL cluster */
	if(sxc_cluster_fetch_ca(cluster, args.batch_mode_flag)) {
            fprintf(stderr, "ERROR: Failed to fetch cluster CA: %s\n", sxc_geterrmsg(sx));
	    goto init_err;
        }
    }

    if(!args.config_link_given) {
        unsigned int toklen;
        char tok_buf[AUTHTOK_ASCII_LEN+1];

        if(args.auth_file_given && strcmp(args.auth_file_arg, "-")) {
            FILE *f = fopen(args.auth_file_arg, "r");
            if(!f) {
                fprintf(stderr, "ERROR: Failed to open key file %s\n", args.auth_file_arg);
                goto init_err;
            }
            c.token = fgets(tok_buf, sizeof(tok_buf), f);
            fclose(f);
        } else if(args.key_given) {
            printf("Please enter the user key: ");
            c.token = fgets(tok_buf, sizeof(tok_buf), stdin);
        } else { /* No key nor auth file given, prompt for password */
            char pass[1024], *p = NULL;

            if(!sxc_cluster_get_uuid(cluster)) {
                /* Send a bogus query in order to obtain cluster UUID */
                if(fetch_cluster_noauth_info(cluster, c.uri))
                    goto init_err; /* Error message has already been printed */

                if(!sxc_cluster_get_uuid(cluster)) {
                    fprintf(stderr, "ERROR: Failed to obtain cluster UUID\n");
                    goto init_err;
                }
            }

            mlock(pass, sizeof(pass));
            if(args.pass_file_given && strcmp(args.pass_file_arg, "-")) {
                FILE *f = fopen(args.pass_file_arg, "r");
                unsigned int len;

                if(!f) {
                    fprintf(stderr, "ERROR: Failed to open pass file %s\n", args.pass_file_arg);
                    munlock(pass, sizeof(pass));
                    goto init_err;
                }
                p = fgets(pass, sizeof(pass), f);
                fclose(f);

                if(!p) {
                    fprintf(stderr, "ERROR: Failed to read pass file %s\n", args.pass_file_arg);
                    memset(pass, 0, sizeof(pass));
                    munlock(pass, sizeof(pass));
                    goto init_err;
                }

                len = strlen(p);
                if(len && p[len-1] == '\n')
                    p[len-1] = '\0';
            }

            /* If p is NULL, user will be prompted for a password, otherwise pass buffer will be used */
            if(sxc_pass2token(cluster, c.uri->profile, p, tok_buf, sizeof(tok_buf))) {
                fprintf(stderr, "ERROR: Failed to get authentication token: %s\n", sxc_geterrmsg(sx));
                memset(pass, 0, sizeof(pass));
                munlock(pass, sizeof(pass));
                goto init_err;
            }
            memset(pass, 0, sizeof(pass));
            munlock(pass, sizeof(pass));
            c.token = tok_buf;
        }

        toklen = strlen(c.token);
        if(toklen && c.token[toklen - 1] == '\n')
            c.token[toklen] = '\0';
    }

    if(!c.token) {
        fprintf(stderr, "ERROR: Failed to read user key\n");
        goto init_err;
    }

    if(!strncmp("CLUSTER/ALLNODE/ROOT/USER", c.token, lenof("CLUSTER/ALLNODE/ROOT/USER"))) {
	fprintf(stderr, "ERROR: The token provided is a cluster identificator and cannot be used for user authentication\n");
	goto init_err;
    }

    if(sxc_cluster_add_access(cluster, c.uri->profile, c.token) ||
       sxc_cluster_set_access(cluster, c.uri->profile)) {
	fprintf(stderr, "ERROR: Failed to set profile authentication: %s\n", sxc_geterrmsg(sx));
	goto init_err;
    }

    if(sxc_cluster_fetchnodes(cluster)) {
	fprintf(stderr, "ERROR: Failed to retrieve cluster members: %s\n", sxc_geterrmsg(sx));
	goto init_err;
    }

    if(args.force_reinit_flag) {
	if(sxc_cluster_remove(cluster, args.config_dir_arg)) {
	    fprintf(stderr, "ERROR: Failed to remove the existing access configuration: %s\n", sxc_geterrmsg(sx));
	    goto init_err;
	}
    }

    if(sxc_cluster_save(cluster, args.config_dir_arg)) {
	fprintf(stderr, "ERROR: Failed to save the access configuration: %s\n", sxc_geterrmsg(sx));
	goto init_err;
    }

    if(args.alias_given) {
        const char *profile;
        if(!c.uri->profile || !c.uri->profile[0])
            profile = "default";
        else
            profile = c.uri->profile;

        /* Save alias into .aliases file. Alias variable was set before. */
        if(sxc_set_alias(sx, c.alias, profile, c.uri->host)) {
            fprintf(stderr, "ERROR: Failed to set alias %s: %s\n", c.alias, sxc_geterrmsg(sx));
            goto init_err;
        }
    }

    ret = 0;
 init_err:
    if(sx && ret) {
	if(c.uri && strstr(sxc_geterrmsg(sx), SXBC_SXINIT_RESOLVE_ERR))
	    fprintf(stderr, SXBC_SXINIT_RESOLVE_MSG, c.uri->host, c.uri->host);
	else if(strstr(sxc_geterrmsg(sx), SXBC_SXINIT_UUID_ERR))
	    fprintf(stderr, SXBC_SXINIT_UUID_MSG);
    }
    sxc_free_uri(c.uri);
    sxc_cluster_free(cluster);
    free(link);
    free(c.hostlist);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    sxc_shutdown(sx, 0);
    cmdline_parser_free(&args);
    return ret;
}
