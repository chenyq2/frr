/*
 * CLI backend interface.
 *
 * --
 * Copyright (C) 2016 Cumulus Networks, Inc.
 * Copyright (C) 1997, 98, 99 Kunihiro Ishiguro
 * Copyright (C) 2013 by Open Source Routing.
 * Copyright (C) 2013 by Internet Systems Consortium, Inc. ("ISC")
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>


#include "memory.h"
#include "log.h"
#include "log_int.h"
#include <lib/version.h>
#include "thread.h"
#include "vector.h"
#include "linklist.h"
#include "vty.h"
#include "command.h"
#include "workqueue.h"
#include "vrf.h"
#include "command_match.h"
#include "command_graph.h"
#include "qobj.h"
#include "defaults.h"

DEFINE_MTYPE(       LIB, HOST,       "Host config")
DEFINE_MTYPE(       LIB, STRVEC,     "String vector")
DEFINE_MTYPE(       LIB, COMPLETION, "Completion item")

/* Command vector which includes some level of command lists. Normally
   each daemon maintains each own cmdvec. */
vector cmdvec = NULL;

/* Host information structure. */
struct host host;

/* Standard command node structures. */
static struct cmd_node auth_node =
{
  AUTH_NODE,
  "Password: ",
};

static struct cmd_node view_node =
{
  VIEW_NODE,
  "%s> ",
};

static struct cmd_node auth_enable_node =
{
  AUTH_ENABLE_NODE,
  "Password: ",
};

static struct cmd_node enable_node =
{
  ENABLE_NODE,
  "%s# ",
};

static struct cmd_node config_node =
{
  CONFIG_NODE,
  "%s(config)# ",
  1
};

/* Default motd string. */
static const char *default_motd = FRR_DEFAULT_MOTD;

static const struct facility_map {
  int facility;
  const char *name;
  size_t match;
} syslog_facilities[] =
  {
    { LOG_KERN, "kern", 1 },
    { LOG_USER, "user", 2 },
    { LOG_MAIL, "mail", 1 },
    { LOG_DAEMON, "daemon", 1 },
    { LOG_AUTH, "auth", 1 },
    { LOG_SYSLOG, "syslog", 1 },
    { LOG_LPR, "lpr", 2 },
    { LOG_NEWS, "news", 1 },
    { LOG_UUCP, "uucp", 2 },
    { LOG_CRON, "cron", 1 },
#ifdef LOG_FTP
    { LOG_FTP, "ftp", 1 },
#endif
    { LOG_LOCAL0, "local0", 6 },
    { LOG_LOCAL1, "local1", 6 },
    { LOG_LOCAL2, "local2", 6 },
    { LOG_LOCAL3, "local3", 6 },
    { LOG_LOCAL4, "local4", 6 },
    { LOG_LOCAL5, "local5", 6 },
    { LOG_LOCAL6, "local6", 6 },
    { LOG_LOCAL7, "local7", 6 },
    { 0, NULL, 0 },
  };

static const char *
facility_name(int facility)
{
  const struct facility_map *fm;

  for (fm = syslog_facilities; fm->name; fm++)
    if (fm->facility == facility)
      return fm->name;
  return "";
}

static int
facility_match(const char *str)
{
  const struct facility_map *fm;

  for (fm = syslog_facilities; fm->name; fm++)
    if (!strncmp(str,fm->name,fm->match))
      return fm->facility;
  return -1;
}

static int
level_match(const char *s)
{
  int level ;

  for ( level = 0 ; zlog_priority [level] != NULL ; level ++ )
    if (!strncmp (s, zlog_priority[level], 2))
      return level;
  return ZLOG_DISABLED;
}

/* This is called from main when a daemon is invoked with -v or --version. */
void
print_version (const char *progname)
{
  printf ("%s version %s\n", progname, FRR_VERSION);
  printf ("%s\n", FRR_COPYRIGHT);
  printf ("configured with:\n\t%s\n", FRR_CONFIG_ARGS);
}


/* Utility function to concatenate argv argument into a single string
   with inserting ' ' character between each argument.  */
char *
argv_concat (struct cmd_token **argv, int argc, int shift)
{
  int i;
  size_t len;
  char *str;
  char *p;

  len = 0;
  for (i = shift; i < argc; i++)
    len += strlen(argv[i]->arg)+1;
  if (!len)
    return NULL;
  p = str = XMALLOC(MTYPE_TMP, len);
  for (i = shift; i < argc; i++)
    {
      size_t arglen;
      memcpy(p, argv[i]->arg, (arglen = strlen(argv[i]->arg)));
      p += arglen;
      *p++ = ' ';
    }
  *(p-1) = '\0';
  return str;
}

/**
 * Convenience function for accessing argv data.
 *
 * @param argc
 * @param argv
 * @param text definition snippet of the desired token
 * @param index the starting index, and where to store the
 *        index of the found token if it exists
 * @return 1 if found, 0 otherwise
 */
int
argv_find (struct cmd_token **argv, int argc, const char *text, int *index)
{
  int found = 0;
  for (int i = *index; i < argc && found == 0; i++)
    if ((found = strmatch (text, argv[i]->text)))
      *index = i;
  return found;
}

static unsigned int
cmd_hash_key (void *p)
{
  return (uintptr_t) p;
}

static int
cmd_hash_cmp (const void *a, const void *b)
{
  return a == b;
}

/* Install top node of command vector. */
void
install_node (struct cmd_node *node,
              int (*func) (struct vty *))
{
  vector_set_index (cmdvec, node->node, node);
  node->func = func;
  node->cmdgraph = graph_new ();
  node->cmd_vector = vector_init (VECTOR_MIN_SIZE);
  // add start node
  struct cmd_token *token = cmd_token_new (START_TKN, CMD_ATTR_NORMAL, NULL, NULL);
  graph_new_node (node->cmdgraph, token, (void (*)(void *)) &cmd_token_del);
  node->cmd_hash = hash_create (cmd_hash_key, cmd_hash_cmp);
}

/**
 * Tokenizes a string, storing tokens in a vector.
 * Whitespace is ignored.
 *
 * Delimiter string = " \n\r\t".
 *
 * @param string to tokenize
 * @return tokenized string
 */
vector
cmd_make_strvec (const char *string)
{
  if (!string) return NULL;

  char *copy, *copystart;
  copystart = copy = XSTRDUP (MTYPE_TMP, string);

  // skip leading whitespace
  while (isspace ((int) *copy) && *copy != '\0') copy++;

  // if the entire string was whitespace or a comment, return
  if (*copy == '\0' || *copy == '!' || *copy == '#')
  {
    XFREE (MTYPE_TMP, copystart);
    return NULL;
  }

  vector strvec = vector_init (VECTOR_MIN_SIZE);
  const char *delim = " \n\r\t", *tok = NULL;
  while (copy)
  {
    tok = strsep (&copy, delim);
    if (*tok != '\0')
      vector_set (strvec, XSTRDUP (MTYPE_STRVEC, tok));
  }

  XFREE (MTYPE_TMP, copystart);
  return strvec;
}

/* Free allocated string vector. */
void
cmd_free_strvec (vector v)
{
  unsigned int i;
  char *cp;

  if (!v)
    return;

  for (i = 0; i < vector_active (v); i++)
    if ((cp = vector_slot (v, i)) != NULL)
      XFREE (MTYPE_STRVEC, cp);

  vector_free (v);
}

/* Return prompt character of specified node. */
const char *
cmd_prompt (enum node_type node)
{
  struct cmd_node *cnode;

  cnode = vector_slot (cmdvec, node);
  return cnode->prompt;
}

/* Install a command into a node. */
void
install_element (enum node_type ntype, struct cmd_element *cmd)
{
  struct cmd_node *cnode;

  /* cmd_init hasn't been called */
  if (!cmdvec)
    {
      fprintf (stderr, "%s called before cmd_init, breakage likely\n",
               __func__);
      return;
    }

  cnode = vector_slot (cmdvec, ntype);

  if (cnode == NULL)
    {
      fprintf (stderr, "Command node %d doesn't exist, please check it\n",
               ntype);
      fprintf (stderr, "Have you called install_node before this install_element?\n");
      exit (EXIT_FAILURE);
    }

  if (hash_lookup (cnode->cmd_hash, cmd) != NULL)
    {
      fprintf (stderr,
               "Multiple command installs to node %d of command:\n%s\n",
               ntype, cmd->string);
      return;
    }

  assert (hash_get (cnode->cmd_hash, cmd, hash_alloc_intern));

  struct graph *graph = graph_new();
  struct cmd_token *token = cmd_token_new (START_TKN, CMD_ATTR_NORMAL, NULL, NULL);
  graph_new_node (graph, token, (void (*)(void *)) &cmd_token_del);

  cmd_graph_parse (graph, cmd);
  cmd_graph_names (graph);
  cmd_graph_merge (cnode->cmdgraph, graph, +1);
  graph_delete_graph (graph);

  vector_set (cnode->cmd_vector, cmd);

  if (ntype == VIEW_NODE)
    install_element (ENABLE_NODE, cmd);
}

void
uninstall_element (enum node_type ntype, struct cmd_element *cmd)
{
  struct cmd_node *cnode;

  /* cmd_init hasn't been called */
  if (!cmdvec)
    {
      fprintf (stderr, "%s called before cmd_init, breakage likely\n",
               __func__);
      return;
    }

  cnode = vector_slot (cmdvec, ntype);

  if (cnode == NULL)
    {
      fprintf (stderr, "Command node %d doesn't exist, please check it\n",
               ntype);
      fprintf (stderr, "Have you called install_node before this install_element?\n");
      exit (EXIT_FAILURE);
    }

  if (hash_release (cnode->cmd_hash, cmd) == NULL)
    {
      fprintf (stderr,
               "Trying to uninstall non-installed command (node %d):\n%s\n",
               ntype, cmd->string);
      return;
    }

  vector_unset_value (cnode->cmd_vector, cmd);

  struct graph *graph = graph_new();
  struct cmd_token *token = cmd_token_new (START_TKN, CMD_ATTR_NORMAL, NULL, NULL);
  graph_new_node (graph, token, (void (*)(void *)) &cmd_token_del);

  cmd_graph_parse (graph, cmd);
  cmd_graph_names (graph);
  cmd_graph_merge (cnode->cmdgraph, graph, -1);
  graph_delete_graph (graph);

  if (ntype == VIEW_NODE)
    uninstall_element (ENABLE_NODE, cmd);
}


static const unsigned char itoa64[] =
"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void
to64(char *s, long v, int n)
{
  while (--n >= 0)
    {
      *s++ = itoa64[v&0x3f];
      v >>= 6;
    }
}

static char *
zencrypt (const char *passwd)
{
  char salt[6];
  struct timeval tv;
  char *crypt (const char *, const char *);

  gettimeofday(&tv,0);

  to64(&salt[0], random(), 3);
  to64(&salt[3], tv.tv_usec, 3);
  salt[5] = '\0';

  return crypt (passwd, salt);
}

/* This function write configuration of this host. */
static int
config_write_host (struct vty *vty)
{
  if (host.name)
    vty_out (vty, "hostname %s%s", host.name, VTY_NEWLINE);

  if (host.encrypt)
    {
      if (host.password_encrypt)
        vty_out (vty, "password 8 %s%s", host.password_encrypt, VTY_NEWLINE);
      if (host.enable_encrypt)
        vty_out (vty, "enable password 8 %s%s", host.enable_encrypt, VTY_NEWLINE);
    }
  else
    {
      if (host.password)
        vty_out (vty, "password %s%s", host.password, VTY_NEWLINE);
      if (host.enable)
        vty_out (vty, "enable password %s%s", host.enable, VTY_NEWLINE);
    }

  if (zlog_default->default_lvl != LOG_DEBUG)
    {
      vty_out (vty, "! N.B. The 'log trap' command is deprecated.%s",
               VTY_NEWLINE);
      vty_out (vty, "log trap %s%s",
               zlog_priority[zlog_default->default_lvl], VTY_NEWLINE);
    }

  if (host.logfile && (zlog_default->maxlvl[ZLOG_DEST_FILE] != ZLOG_DISABLED))
    {
      vty_out (vty, "log file %s", host.logfile);
      if (zlog_default->maxlvl[ZLOG_DEST_FILE] != zlog_default->default_lvl)
        vty_out (vty, " %s",
                 zlog_priority[zlog_default->maxlvl[ZLOG_DEST_FILE]]);
      vty_out (vty, "%s", VTY_NEWLINE);
    }

  if (zlog_default->maxlvl[ZLOG_DEST_STDOUT] != ZLOG_DISABLED)
    {
      vty_out (vty, "log stdout");
      if (zlog_default->maxlvl[ZLOG_DEST_STDOUT] != zlog_default->default_lvl)
        vty_out (vty, " %s",
                 zlog_priority[zlog_default->maxlvl[ZLOG_DEST_STDOUT]]);
      vty_out (vty, "%s", VTY_NEWLINE);
    }

  if (zlog_default->maxlvl[ZLOG_DEST_MONITOR] == ZLOG_DISABLED)
    vty_out(vty,"no log monitor%s",VTY_NEWLINE);
  else if (zlog_default->maxlvl[ZLOG_DEST_MONITOR] != zlog_default->default_lvl)
    vty_out(vty,"log monitor %s%s",
            zlog_priority[zlog_default->maxlvl[ZLOG_DEST_MONITOR]],VTY_NEWLINE);

  if (zlog_default->maxlvl[ZLOG_DEST_SYSLOG] != ZLOG_DISABLED)
    {
      vty_out (vty, "log syslog");
      if (zlog_default->maxlvl[ZLOG_DEST_SYSLOG] != zlog_default->default_lvl)
        vty_out (vty, " %s",
                 zlog_priority[zlog_default->maxlvl[ZLOG_DEST_SYSLOG]]);
      vty_out (vty, "%s", VTY_NEWLINE);
    }

  if (zlog_default->facility != LOG_DAEMON)
    vty_out (vty, "log facility %s%s",
             facility_name(zlog_default->facility), VTY_NEWLINE);

  if (zlog_default->record_priority == 1)
    vty_out (vty, "log record-priority%s", VTY_NEWLINE);

  if (zlog_default->timestamp_precision > 0)
    vty_out (vty, "log timestamp precision %d%s",
             zlog_default->timestamp_precision, VTY_NEWLINE);

  if (host.advanced)
    vty_out (vty, "service advanced-vty%s", VTY_NEWLINE);

  if (host.encrypt)
    vty_out (vty, "service password-encryption%s", VTY_NEWLINE);

  if (host.lines >= 0)
    vty_out (vty, "service terminal-length %d%s", host.lines,
             VTY_NEWLINE);

  if (host.motdfile)
    vty_out (vty, "banner motd file %s%s", host.motdfile, VTY_NEWLINE);
  else if (! host.motd)
    vty_out (vty, "no banner motd%s", VTY_NEWLINE);

  return 1;
}

/* Utility function for getting command graph. */
static struct graph *
cmd_node_graph (vector v, enum node_type ntype)
{
  struct cmd_node *cnode = vector_slot (v, ntype);
  return cnode->cmdgraph;
}

static int
cmd_try_do_shortcut (enum node_type node, char* first_word) {
  if ( first_word != NULL &&
       node != AUTH_NODE &&
       node != VIEW_NODE &&
       node != AUTH_ENABLE_NODE &&
       node != ENABLE_NODE &&
       0 == strcmp( "do", first_word ) )
    return 1;
  return 0;
}

/**
 * Compare function for cmd_token.
 * Used with qsort to sort command completions.
 */
static int
compare_completions (const void *fst, const void *snd)
{
  struct cmd_token *first = *(struct cmd_token **) fst,
                     *secnd = *(struct cmd_token **) snd;
  return strcmp (first->text, secnd->text);
}

/**
 * Takes a list of completions returned by command_complete,
 * dedeuplicates them based on both text and description,
 * sorts them, and returns them as a vector.
 *
 * @param completions linked list of cmd_token
 * @return deduplicated and sorted vector with
 */
vector
completions_to_vec (struct list *completions)
{
  vector comps = vector_init (VECTOR_MIN_SIZE);

  struct listnode *ln;
  struct cmd_token *token, *cr = NULL;
  unsigned int i, exists;
  for (ALL_LIST_ELEMENTS_RO(completions,ln,token))
  {
    if (token->type == END_TKN && (cr = token))
      continue;

    // linear search for token in completions vector
    exists = 0;
    for (i = 0; i < vector_active (comps) && !exists; i++)
    {
      struct cmd_token *curr = vector_slot (comps, i);
#ifdef VTYSH_DEBUG
      exists = !strcmp (curr->text, token->text) &&
               !strcmp (curr->desc, token->desc);
#else
      exists = !strcmp (curr->text, token->text);
#endif /* VTYSH_DEBUG */
    }

    if (!exists)
      vector_set (comps, token);
  }

  // sort completions
  qsort (comps->index,
         vector_active (comps),
         sizeof (void *),
         &compare_completions);

  // make <cr> the first element, if it is present
  if (cr)
  {
    vector_set_index (comps, vector_active (comps), NULL);
    memmove (comps->index + 1, comps->index, (comps->alloced - 1) * sizeof (void *));
    vector_set_index (comps, 0, cr);
  }

  return comps;
}
/**
 * Generates a vector of cmd_token representing possible completions
 * on the current input.
 *
 * @param vline the vectorized input line
 * @param vty the vty with the node to match on
 * @param status pointer to matcher status code
 * @return vector of struct cmd_token * with possible completions
 */
static vector
cmd_complete_command_real (vector vline, struct vty *vty, int *status)
{
  struct list *completions;
  struct graph *cmdgraph = cmd_node_graph (cmdvec, vty->node);

  enum matcher_rv rv = command_complete (cmdgraph, vline, &completions);

  if (MATCHER_ERROR(rv))
  {
    *status = CMD_ERR_NO_MATCH;
    return NULL;
  }

  vector comps = completions_to_vec (completions);
  list_delete (completions);

  // set status code appropriately
  switch (vector_active (comps))
  {
    case 0:
      *status = CMD_ERR_NO_MATCH;
      break;
    case 1:
      *status = CMD_COMPLETE_FULL_MATCH;
      break;
    default:
      *status = CMD_COMPLETE_LIST_MATCH;
  }

  return comps;
}

vector
cmd_describe_command (vector vline, struct vty *vty, int *status)
{
  vector ret;

  if ( cmd_try_do_shortcut(vty->node, vector_slot(vline, 0) ) )
    {
      enum node_type onode;
      vector shifted_vline;
      unsigned int index;

      onode = vty->node;
      vty->node = ENABLE_NODE;
      /* We can try it on enable node, cos' the vty is authenticated */

      shifted_vline = vector_init (vector_count(vline));
      /* use memcpy? */
      for (index = 1; index < vector_active (vline); index++)
        {
          vector_set_index (shifted_vline, index-1, vector_lookup(vline, index));
        }

      ret = cmd_complete_command_real (shifted_vline, vty, status);

      vector_free(shifted_vline);
      vty->node = onode;
      return ret;
  }

  return cmd_complete_command_real (vline, vty, status);
}

static struct list *varhandlers = NULL;

void
cmd_variable_complete (struct cmd_token *token, const char *arg, vector comps)
{
  struct listnode *ln;
  const struct cmd_variable_handler *cvh;
  size_t i, argsz;
  vector tmpcomps;

  tmpcomps = arg ? vector_init (VECTOR_MIN_SIZE) : comps;

  for (ALL_LIST_ELEMENTS_RO(varhandlers, ln, cvh))
    {
      if (cvh->tokenname && strcmp(cvh->tokenname, token->text))
        continue;
      if (cvh->varname && (!token->varname || strcmp(cvh->varname, token->varname)))
        continue;
      cvh->completions(tmpcomps, token);
      break;
    }

  if (!arg)
    return;

  argsz = strlen(arg);
  for (i = vector_active(tmpcomps); i; i--)
    {
      char *item = vector_slot(tmpcomps, i - 1);
      if (strlen(item) >= argsz
          && !strncmp(item, arg, argsz))
        vector_set(comps, item);
      else
        XFREE(MTYPE_COMPLETION, item);
    }
  vector_free(tmpcomps);
}

void
cmd_variable_handler_register (const struct cmd_variable_handler *cvh)
{
  if (!varhandlers)
    return;

  for (; cvh->completions; cvh++)
    listnode_add(varhandlers, (void *)cvh);
}

DEFUN_HIDDEN (autocomplete,
              autocomplete_cmd,
              "autocomplete TYPE TEXT VARNAME",
              "Autocompletion handler (internal, for vtysh)\n"
              "cmd_token->type\n"
              "cmd_token->text\n"
              "cmd_token->varname\n")
{
  struct cmd_token tok;
  vector comps = vector_init(32);
  size_t i;

  memset(&tok, 0, sizeof(tok));
  tok.type = atoi(argv[1]->arg);
  tok.text = argv[2]->arg;
  tok.varname = argv[3]->arg;
  if (!strcmp(tok.varname, "-"))
    tok.varname = NULL;

  cmd_variable_complete(&tok, NULL, comps);

  for (i = 0; i < vector_active(comps); i++)
    {
      char *text = vector_slot(comps, i);
      vty_out(vty, "%s\n", text);
      XFREE(MTYPE_COMPLETION, text);
    }

  vector_free(comps);
  return CMD_SUCCESS;
}

/**
 * Generate possible tab-completions for the given input. This function only
 * returns results that would result in a valid command if used as Readline
 * completions (as is the case in vtysh). For instance, if the passed vline ends
 * with '4.3.2', the strings 'A.B.C.D' and 'A.B.C.D/M' will _not_ be returned.
 *
 * @param vline vectorized input line
 * @param vty the vty
 * @param status location to store matcher status code in
 * @return set of valid strings for use with Readline as tab-completions.
 */

char **
cmd_complete_command (vector vline, struct vty *vty, int *status)
{
  char **ret = NULL;
  int original_node = vty->node;
  vector input_line = vector_init (vector_count (vline));

  // if the first token is 'do' we'll want to execute the command in the enable node
  int do_shortcut = cmd_try_do_shortcut (vty->node, vector_slot (vline, 0));
  vty->node = do_shortcut ? ENABLE_NODE : original_node;

  // construct the input line we'll be matching on
  unsigned int offset = (do_shortcut) ? 1 : 0;
  for (unsigned index = 0; index + offset < vector_active (vline); index++)
    vector_set_index (input_line, index, vector_lookup (vline, index + offset));

  // get token completions -- this is a copying operation
  vector comps = NULL, initial_comps;
  initial_comps = cmd_complete_command_real (input_line, vty, status);

  if (!MATCHER_ERROR (*status))
  {
    assert (initial_comps);
    // filter out everything that is not suitable for a tab-completion
    comps = vector_init (VECTOR_MIN_SIZE);
    for (unsigned int i = 0; i < vector_active(initial_comps); i++)
    {
      struct cmd_token *token = vector_slot (initial_comps, i);
      if (token->type == WORD_TKN)
        vector_set (comps, XSTRDUP (MTYPE_COMPLETION, token->text));
      else if (IS_VARYING_TOKEN(token->type))
        {
          const char *ref = vector_lookup(vline, vector_active (vline) - 1);
          cmd_variable_complete (token, ref, comps);
        }
    }
    vector_free (initial_comps);

    // since we filtered results, we need to re-set status code
    switch (vector_active (comps))
    {
      case 0:
        *status = CMD_ERR_NO_MATCH;
        break;
      case 1:
        *status = CMD_COMPLETE_FULL_MATCH;
        break;
      default:
        *status = CMD_COMPLETE_LIST_MATCH;
    }

    // copy completions text into an array of char*
    ret = XMALLOC (MTYPE_TMP, (vector_active (comps)+1) * sizeof (char *));
    unsigned int i;
    for (i = 0; i < vector_active (comps); i++)
      {
        ret[i] = vector_slot (comps, i);
      }
    // set the last element to NULL, because this array is used in
    // a Readline completion_generator function which expects NULL
    // as a sentinel value
    ret[i] = NULL;
    vector_free (comps);
    comps = NULL;
  }
  else if (initial_comps)
    vector_free (initial_comps);

  // comps should always be null here
  assert (!comps);

  // free the adjusted input line
  vector_free (input_line);

  // reset vty->node to its original value
  vty->node = original_node;

  return ret;
}

/* return parent node */
/* MUST eventually converge on CONFIG_NODE */
enum node_type
node_parent ( enum node_type node )
{
  enum node_type ret;

  assert (node > CONFIG_NODE);

  switch (node)
    {
    case BGP_VPNV4_NODE:
    case BGP_VPNV6_NODE:
    case BGP_ENCAP_NODE:
    case BGP_ENCAPV6_NODE:
    case BGP_VRF_POLICY_NODE:
    case BGP_VNC_DEFAULTS_NODE:
    case BGP_VNC_NVE_GROUP_NODE:
    case BGP_VNC_L2_GROUP_NODE:
    case BGP_IPV4_NODE:
    case BGP_IPV4M_NODE:
    case BGP_IPV4L_NODE:
    case BGP_IPV6_NODE:
    case BGP_IPV6M_NODE:
    case BGP_EVPN_NODE:
    case BGP_IPV6L_NODE:
      ret = BGP_NODE;
      break;
    case KEYCHAIN_KEY_NODE:
      ret = KEYCHAIN_NODE;
      break;
    case LINK_PARAMS_NODE:
      ret = INTERFACE_NODE;
      break;
    case LDP_IPV4_NODE:
    case LDP_IPV6_NODE:
      ret = LDP_NODE;
      break;
    case LDP_IPV4_IFACE_NODE:
      ret = LDP_IPV4_NODE;
      break;
    case LDP_IPV6_IFACE_NODE:
      ret = LDP_IPV6_NODE;
      break;
    case LDP_PSEUDOWIRE_NODE:
      ret = LDP_L2VPN_NODE;
      break;
    default:
      ret = CONFIG_NODE;
      break;
    }

  return ret;
}

/* Execute command by argument vline vector. */
static int
cmd_execute_command_real (vector vline,
                          enum filter_type filter,
                          struct vty *vty,
                          const struct cmd_element **cmd)
{
  struct list *argv_list;
  enum matcher_rv status;
  const struct cmd_element *matched_element = NULL;

  struct graph *cmdgraph = cmd_node_graph (cmdvec, vty->node);
  status = command_match (cmdgraph, vline, &argv_list, &matched_element);

  if (cmd)
    *cmd = matched_element;

  // if matcher error, return corresponding CMD_ERR
  if (MATCHER_ERROR(status))
  {
    if (argv_list)
      list_delete (argv_list);
    switch (status)
    {
      case MATCHER_INCOMPLETE:
        return CMD_ERR_INCOMPLETE;
      case MATCHER_AMBIGUOUS:
        return CMD_ERR_AMBIGUOUS;
      default:
        return CMD_ERR_NO_MATCH;
    }
  }

  // build argv array from argv list
  struct cmd_token **argv = XMALLOC (MTYPE_TMP, argv_list->count * sizeof (struct cmd_token *));
  struct listnode *ln;
  struct cmd_token *token;
  unsigned int i = 0;
  for (ALL_LIST_ELEMENTS_RO(argv_list,ln,token))
    argv[i++] = token;

  int argc = argv_list->count;

  int ret;
  if (matched_element->daemon)
    ret = CMD_SUCCESS_DAEMON;
  else
    ret = matched_element->func (matched_element, vty, argc, argv);

  // delete list and cmd_token's in it
  list_delete (argv_list);
  XFREE (MTYPE_TMP, argv);

  return ret;
}

/**
 * Execute a given command, handling things like "do ..." and checking
 * whether the given command might apply at a parent node if doesn't
 * apply for the current node.
 *
 * @param vline Command line input, vector of char* where each element is
 *              one input token.
 * @param vty The vty context in which the command should be executed.
 * @param cmd Pointer where the struct cmd_element of the matched command
 *            will be stored, if any. May be set to NULL if this info is
 *            not needed.
 * @param vtysh If set != 0, don't lookup the command at parent nodes.
 * @return The status of the command that has been executed or an error code
 *         as to why no command could be executed.
 */
int
cmd_execute_command (vector vline, struct vty *vty,
                     const struct cmd_element **cmd,
                     int vtysh)
{
  int ret, saved_ret = 0;
  enum node_type onode, try_node;

  onode = try_node = vty->node;

  if (cmd_try_do_shortcut(vty->node, vector_slot(vline, 0)))
    {
      vector shifted_vline;
      unsigned int index;

      vty->node = ENABLE_NODE;
      /* We can try it on enable node, cos' the vty is authenticated */

      shifted_vline = vector_init (vector_count(vline));
      /* use memcpy? */
      for (index = 1; index < vector_active (vline); index++)
        vector_set_index (shifted_vline, index-1, vector_lookup(vline, index));

      ret = cmd_execute_command_real (shifted_vline, FILTER_RELAXED, vty, cmd);

      vector_free(shifted_vline);
      vty->node = onode;
      return ret;
  }

  saved_ret = ret = cmd_execute_command_real (vline, FILTER_RELAXED, vty, cmd);

  if (vtysh)
    return saved_ret;

  if (ret != CMD_SUCCESS && ret != CMD_WARNING)
    {
      /* This assumes all nodes above CONFIG_NODE are childs of CONFIG_NODE */
      while (vty->node > CONFIG_NODE)
        {
          try_node = node_parent(try_node);
          vty->node = try_node;
          ret = cmd_execute_command_real (vline, FILTER_RELAXED, vty, cmd);
          if (ret == CMD_SUCCESS || ret == CMD_WARNING)
            return ret;
        }
      /* no command succeeded, reset the vty to the original node */
      vty->node = onode;
    }

  /* return command status for original node */
  return saved_ret;
}

/**
 * Execute a given command, matching it strictly against the current node.
 * This mode is used when reading config files.
 *
 * @param vline Command line input, vector of char* where each element is
 *              one input token.
 * @param vty The vty context in which the command should be executed.
 * @param cmd Pointer where the struct cmd_element* of the matched command
 *            will be stored, if any. May be set to NULL if this info is
 *            not needed.
 * @return The status of the command that has been executed or an error code
 *         as to why no command could be executed.
 */
int
cmd_execute_command_strict (vector vline, struct vty *vty,
                            const struct cmd_element **cmd)
{
  return cmd_execute_command_real(vline, FILTER_STRICT, vty, cmd);
}

/**
 * Parse one line of config, walking up the parse tree attempting to find a match
 *
 * @param vty The vty context in which the command should be executed.
 * @param cmd Pointer where the struct cmd_element* of the match command
 *            will be stored, if any.  May be set to NULL if this info is
 *            not needed.
 * @param use_daemon Boolean to control whether or not we match on CMD_SUCCESS_DAEMON
 *                   or not.
 * @return The status of the command that has been executed or an error code
 *         as to why no command could be executed.
 */
int
command_config_read_one_line (struct vty *vty, const struct cmd_element **cmd, int use_daemon)
{
  vector vline;
  int saved_node;
  int ret;

  vline = cmd_make_strvec (vty->buf);

  /* In case of comment line */
  if (vline == NULL)
    return CMD_SUCCESS;

  /* Execute configuration command : this is strict match */
  ret = cmd_execute_command_strict (vline, vty, cmd);

  // Climb the tree and try the command again at each node
  if (!(use_daemon && ret == CMD_SUCCESS_DAEMON) &&
      !(!use_daemon && ret == CMD_ERR_NOTHING_TODO) &&
      ret != CMD_SUCCESS &&
      ret != CMD_WARNING &&
      vty->node != CONFIG_NODE) {

    saved_node = vty->node;

    while (!(use_daemon && ret == CMD_SUCCESS_DAEMON) &&
           !(!use_daemon && ret == CMD_ERR_NOTHING_TODO) &&
           ret != CMD_SUCCESS &&
           ret != CMD_WARNING &&
           vty->node > CONFIG_NODE) {
      vty->node = node_parent(vty->node);
      ret = cmd_execute_command_strict (vline, vty, cmd);
    }

    // If climbing the tree did not work then ignore the command and
    // stay at the same node
    if (!(use_daemon && ret == CMD_SUCCESS_DAEMON) &&
        !(!use_daemon && ret == CMD_ERR_NOTHING_TODO) &&
        ret != CMD_SUCCESS &&
        ret != CMD_WARNING)
      {
        vty->node = saved_node;
      }
  }

  if (ret != CMD_SUCCESS && ret != CMD_WARNING)
    memcpy (vty->error_buf, vty->buf, VTY_BUFSIZ);

  cmd_free_strvec (vline);

  return ret;
}

/* Configuration make from file. */
int
config_from_file (struct vty *vty, FILE *fp, unsigned int *line_num)
{
  int ret, error_ret=0;
  *line_num = 0;

  while (fgets (vty->buf, VTY_BUFSIZ, fp))
    {
      if (!error_ret)
        ++(*line_num);

      ret = command_config_read_one_line (vty, NULL, 0);

      if (ret != CMD_SUCCESS && ret != CMD_WARNING &&
          ret != CMD_ERR_NOTHING_TODO)
        error_ret = ret;
    }

  if (error_ret) {
      return error_ret;
  }

  return CMD_SUCCESS;
}

/* Configuration from terminal */
DEFUN (config_terminal,
       config_terminal_cmd,
       "configure terminal",
       "Configuration from vty interface\n"
       "Configuration terminal\n")
{
  if (vty_config_lock (vty))
    vty->node = CONFIG_NODE;
  else
    {
      vty_out (vty, "VTY configuration is locked by other VTY%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  return CMD_SUCCESS;
}

/* Enable command */
DEFUN (enable,
       config_enable_cmd,
       "enable",
       "Turn on privileged mode command\n")
{
  /* If enable password is NULL, change to ENABLE_NODE */
  if ((host.enable == NULL && host.enable_encrypt == NULL) ||
      vty->type == VTY_SHELL_SERV)
    vty->node = ENABLE_NODE;
  else
    vty->node = AUTH_ENABLE_NODE;

  return CMD_SUCCESS;
}

/* Disable command */
DEFUN (disable,
       config_disable_cmd,
       "disable",
       "Turn off privileged mode command\n")
{
  if (vty->node == ENABLE_NODE)
    vty->node = VIEW_NODE;
  return CMD_SUCCESS;
}

/* Down vty node level. */
DEFUN (config_exit,
       config_exit_cmd,
       "exit",
       "Exit current mode and down to previous mode\n")
{
  cmd_exit (vty);
  return CMD_SUCCESS;
}

void
cmd_exit (struct vty *vty)
{
  switch (vty->node)
    {
    case VIEW_NODE:
    case ENABLE_NODE:
      if (vty_shell (vty))
        exit (0);
      else
        vty->status = VTY_CLOSE;
      break;
    case CONFIG_NODE:
      vty->node = ENABLE_NODE;
      vty_config_unlock (vty);
      break;
    case INTERFACE_NODE:
    case NS_NODE:
    case VRF_NODE:
    case ZEBRA_NODE:
    case BGP_NODE:
    case RIP_NODE:
    case EIGRP_NODE:
    case RIPNG_NODE:
    case OSPF_NODE:
    case OSPF6_NODE:
    case LDP_NODE:
    case LDP_L2VPN_NODE:
    case ISIS_NODE:
    case KEYCHAIN_NODE:
    case MASC_NODE:
    case RMAP_NODE:
    case PIM_NODE:
    case VTY_NODE:
      vty->node = CONFIG_NODE;
      break;
    case BGP_IPV4_NODE:
    case BGP_IPV4M_NODE:
    case BGP_IPV4L_NODE:
    case BGP_VPNV4_NODE:
    case BGP_VPNV6_NODE:
    case BGP_ENCAP_NODE:
    case BGP_ENCAPV6_NODE:
    case BGP_VRF_POLICY_NODE:
    case BGP_VNC_DEFAULTS_NODE:
    case BGP_VNC_NVE_GROUP_NODE:
    case BGP_VNC_L2_GROUP_NODE:
    case BGP_IPV6_NODE:
    case BGP_IPV6M_NODE:
    case BGP_EVPN_NODE:
    case BGP_IPV6L_NODE:
      vty->node = BGP_NODE;
      break;
    case LDP_IPV4_NODE:
    case LDP_IPV6_NODE:
      vty->node = LDP_NODE;
      break;
    case LDP_IPV4_IFACE_NODE:
      vty->node = LDP_IPV4_NODE;
      break;
    case LDP_IPV6_IFACE_NODE:
      vty->node = LDP_IPV6_NODE;
      break;
    case LDP_PSEUDOWIRE_NODE:
      vty->node = LDP_L2VPN_NODE;
      break;
    case KEYCHAIN_KEY_NODE:
      vty->node = KEYCHAIN_NODE;
      break;
    case LINK_PARAMS_NODE:
      vty->node = INTERFACE_NODE;
      break;
    default:
      break;
    }
}

/* ALIAS_FIXME */
DEFUN (config_quit,
       config_quit_cmd,
       "quit",
       "Exit current mode and down to previous mode\n")
{
  return config_exit (self, vty, argc, argv);
}


/* End of configuration. */
DEFUN (config_end,
       config_end_cmd,
       "end",
       "End current mode and change to enable mode.")
{
  switch (vty->node)
    {
    case VIEW_NODE:
    case ENABLE_NODE:
      /* Nothing to do. */
      break;
    case CONFIG_NODE:
    case INTERFACE_NODE:
    case NS_NODE:
    case VRF_NODE:
    case ZEBRA_NODE:
    case RIP_NODE:
    case RIPNG_NODE:
    case EIGRP_NODE:
    case BGP_NODE:
    case BGP_ENCAP_NODE:
    case BGP_ENCAPV6_NODE:
    case BGP_VRF_POLICY_NODE:
    case BGP_VNC_DEFAULTS_NODE:
    case BGP_VNC_NVE_GROUP_NODE:
    case BGP_VNC_L2_GROUP_NODE:
    case BGP_VPNV4_NODE:
    case BGP_VPNV6_NODE:
    case BGP_IPV4_NODE:
    case BGP_IPV4M_NODE:
    case BGP_IPV4L_NODE:
    case BGP_IPV6_NODE:
    case BGP_IPV6M_NODE:
    case BGP_EVPN_NODE:
    case BGP_IPV6L_NODE:
    case RMAP_NODE:
    case OSPF_NODE:
    case OSPF6_NODE:
    case LDP_NODE:
    case LDP_IPV4_NODE:
    case LDP_IPV6_NODE:
    case LDP_IPV4_IFACE_NODE:
    case LDP_IPV6_IFACE_NODE:
    case LDP_L2VPN_NODE:
    case LDP_PSEUDOWIRE_NODE:
    case ISIS_NODE:
    case KEYCHAIN_NODE:
    case KEYCHAIN_KEY_NODE:
    case MASC_NODE:
    case PIM_NODE:
    case VTY_NODE:
    case LINK_PARAMS_NODE:
      vty_config_unlock (vty);
      vty->node = ENABLE_NODE;
      break;
    default:
      break;
    }
  return CMD_SUCCESS;
}

/* Show version. */
DEFUN (show_version,
       show_version_cmd,
       "show version",
       SHOW_STR
       "Displays zebra version\n")
{
  vty_out (vty, "%s %s (%s).%s", FRR_FULL_NAME, FRR_VERSION,
	   host.name ? host.name : "",
	   VTY_NEWLINE);
  vty_out (vty, "%s%s%s", FRR_COPYRIGHT, GIT_INFO, VTY_NEWLINE);
  vty_out (vty, "configured with:%s    %s%s", VTY_NEWLINE,
           FRR_CONFIG_ARGS, VTY_NEWLINE);

  return CMD_SUCCESS;
}

/* "Set" version ... ignore version tags */
DEFUN (frr_version_defaults,
       frr_version_defaults_cmd,
       "frr <version|defaults> LINE...",
       "FRRouting global parameters\n"
       "version configuration was written by\n"
       "set of configuration defaults used\n"
       "version string\n")
{
  return CMD_SUCCESS;
}

/* Help display function for all node. */
DEFUN (config_help,
       config_help_cmd,
       "help",
       "Description of the interactive help system\n")
{
  vty_out (vty,
           "Quagga VTY provides advanced help feature.  When you need help,%s\
anytime at the command line please press '?'.%s\
%s\
If nothing matches, the help list will be empty and you must backup%s\
 until entering a '?' shows the available options.%s\
Two styles of help are provided:%s\
1. Full help is available when you are ready to enter a%s\
command argument (e.g. 'show ?') and describes each possible%s\
argument.%s\
2. Partial help is provided when an abbreviated argument is entered%s\
   and you want to know what arguments match the input%s\
   (e.g. 'show me?'.)%s%s", VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE,
           VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE,
           VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE);
  return CMD_SUCCESS;
}

static void
permute (struct graph_node *start, struct vty *vty)
{
  static struct list *position = NULL;
  if (!position) position = list_new ();

  struct cmd_token *stok = start->data;
  struct graph_node *gnn;
  struct listnode *ln;

  // recursive dfs
  listnode_add (position, start);
  for (unsigned int i = 0; i < vector_active (start->to); i++)
  {
    struct graph_node *gn = vector_slot (start->to, i);
    struct cmd_token *tok = gn->data;
    if (tok->attr == CMD_ATTR_HIDDEN ||
        tok->attr == CMD_ATTR_DEPRECATED)
      continue;
    else if (tok->type == END_TKN || gn == start)
    {
      vty_out (vty, " ");
      for (ALL_LIST_ELEMENTS_RO (position,ln,gnn))
      {
        struct cmd_token *tt = gnn->data;
        if (tt->type < SPECIAL_TKN)
          vty_out (vty, " %s", tt->text);
      }
      if (gn == start)
        vty_out (vty, "...");
      vty_out (vty, VTY_NEWLINE);
    }
    else
    {
      bool skip = false;
      if (stok->type == FORK_TKN && tok->type != FORK_TKN)
        for (ALL_LIST_ELEMENTS_RO (position, ln, gnn))
           if (gnn == gn)
           {
             skip = true;
             break;
           }
      if (!skip)
        permute (gn, vty);
    }
  }
  list_delete_node (position, listtail(position));
}

int
cmd_list_cmds (struct vty *vty, int do_permute)
{
  struct cmd_node *node = vector_slot (cmdvec, vty->node);

  if (do_permute)
    permute (vector_slot (node->cmdgraph->nodes, 0), vty);
  else
  {
    /* loop over all commands at this node */
    struct cmd_element *element = NULL;
    for (unsigned int i = 0; i < vector_active(node->cmd_vector); i++)
        if ((element = vector_slot (node->cmd_vector, i)) &&
             element->attr != CMD_ATTR_DEPRECATED &&
             element->attr != CMD_ATTR_HIDDEN)
          vty_out (vty, "    %s%s", element->string, VTY_NEWLINE);
  }
  return CMD_SUCCESS;
}

/* Help display function for all node. */
DEFUN (config_list,
       config_list_cmd,
       "list [permutations]",
       "Print command list\n"
       "Print all possible command permutations\n")
{
  return cmd_list_cmds (vty, argc == 2);
}

DEFUN (show_commandtree,
       show_commandtree_cmd,
       "show commandtree [permutations]",
       SHOW_STR
       "Show command tree\n"
       "Permutations that we are interested in\n")
{
  return cmd_list_cmds (vty, argc == 3);
}

static void
vty_write_config (struct vty *vty)
{
  size_t i;
  struct cmd_node *node;

  if (vty->type == VTY_TERM)
    {
      vty_out (vty, "%sCurrent configuration:%s", VTY_NEWLINE,
               VTY_NEWLINE);
      vty_out (vty, "!%s", VTY_NEWLINE);
    }

  vty_out (vty, "frr version %s%s", FRR_VER_SHORT, VTY_NEWLINE);
  vty_out (vty, "frr defaults %s%s", DFLT_NAME, VTY_NEWLINE);
  vty_out (vty, "!%s", VTY_NEWLINE);

  for (i = 0; i < vector_active (cmdvec); i++)
    if ((node = vector_slot (cmdvec, i)) && node->func
        && (node->vtysh || vty->type != VTY_SHELL))
      {
        if ((*node->func) (vty))
          vty_out (vty, "!%s", VTY_NEWLINE);
      }

  if (vty->type == VTY_TERM)
    {
      vty_out (vty, "end%s",VTY_NEWLINE);
    }
}

/* Write current configuration into file. */

DEFUN (config_write,
       config_write_cmd,
       "write [<file|memory|terminal>]",
       "Write running configuration to memory, network, or terminal\n"
       "Write to configuration file\n"
       "Write configuration currently in memory\n"
       "Write configuration to terminal\n")
{
  int idx_type = 1;
  int fd, dirfd;
  char *config_file, *slash;
  char *config_file_tmp = NULL;
  char *config_file_sav = NULL;
  int ret = CMD_WARNING;
  struct vty *file_vty;
  struct stat conf_stat;

  // if command was 'write terminal' or 'show running-config'
  if (argc == 2 && (!strcmp(argv[idx_type]->text, "terminal") ||
                    !strcmp(argv[0]->text, "show")))
    {
      vty_write_config (vty);
      return CMD_SUCCESS;
    }

  if (host.noconfig)
    return CMD_SUCCESS;

  /* Check and see if we are operating under vtysh configuration */
  if (host.config == NULL)
    {
      vty_out (vty, "Can't save to configuration file, using vtysh.%s",
               VTY_NEWLINE);
      return CMD_WARNING;
    }

  /* Get filename. */
  config_file = host.config;

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
  slash = strrchr (config_file, '/');
  if (slash)
    {
      char *config_dir = XSTRDUP (MTYPE_TMP, config_file);
      config_dir[slash - config_file] = '\0';
      dirfd = open(config_dir, O_DIRECTORY | O_RDONLY);
      XFREE (MTYPE_TMP, config_dir);
    }
  else
    dirfd = open(".", O_DIRECTORY | O_RDONLY);
  /* if dirfd is invalid, directory sync fails, but we're still OK */

  config_file_sav =
    XMALLOC (MTYPE_TMP, strlen (config_file) + strlen (CONF_BACKUP_EXT) + 1);
  strcpy (config_file_sav, config_file);
  strcat (config_file_sav, CONF_BACKUP_EXT);


  config_file_tmp = XMALLOC (MTYPE_TMP, strlen (config_file) + 8);
  sprintf (config_file_tmp, "%s.XXXXXX", config_file);

  /* Open file to configuration write. */
  fd = mkstemp (config_file_tmp);
  if (fd < 0)
    {
      vty_out (vty, "Can't open configuration file %s.%s", config_file_tmp,
               VTY_NEWLINE);
      goto finished;
    }
  if (fchmod (fd, CONFIGFILE_MASK) != 0)
    {
      vty_out (vty, "Can't chmod configuration file %s: %s (%d).%s",
        config_file_tmp, safe_strerror(errno), errno, VTY_NEWLINE);
      goto finished;
    }

  /* Make vty for configuration file. */
  file_vty = vty_new ();
  file_vty->wfd = fd;
  file_vty->type = VTY_FILE;

  /* Config file header print. */
  vty_out (file_vty, "!\n! Zebra configuration saved from vty\n!   ");
  vty_time_print (file_vty, 1);
  vty_out (file_vty, "!\n");
  vty_write_config (file_vty);
  vty_close (file_vty);

  if (stat(config_file, &conf_stat) >= 0)
    {
      if (unlink (config_file_sav) != 0)
        if (errno != ENOENT)
          {
            vty_out (vty, "Can't unlink backup configuration file %s.%s", config_file_sav,
                     VTY_NEWLINE);
            goto finished;
          }
      if (link (config_file, config_file_sav) != 0)
        {
          vty_out (vty, "Can't backup old configuration file %s.%s", config_file_sav,
                   VTY_NEWLINE);
          goto finished;
        }
      if (dirfd >= 0)
        fsync (dirfd);
    }
  if (rename (config_file_tmp, config_file) != 0)
    {
      vty_out (vty, "Can't save configuration file %s.%s", config_file,
               VTY_NEWLINE);
      goto finished;
    }
  if (dirfd >= 0)
    fsync (dirfd);

  vty_out (vty, "Configuration saved to %s%s", config_file,
           VTY_NEWLINE);
  ret = CMD_SUCCESS;

finished:
  if (ret != CMD_SUCCESS)
    unlink (config_file_tmp);
  if (dirfd >= 0)
    close (dirfd);
  XFREE (MTYPE_TMP, config_file_tmp);
  XFREE (MTYPE_TMP, config_file_sav);
  return ret;
}

/* ALIAS_FIXME for 'write <terminal|memory>' */
DEFUN (show_running_config,
       show_running_config_cmd,
       "show running-config",
       SHOW_STR
       "running configuration (same as write terminal/memory)\n")
{
  return config_write (self, vty, argc, argv);
}

/* ALIAS_FIXME for 'write file' */
DEFUN (copy_runningconf_startupconf,
       copy_runningconf_startupconf_cmd,
       "copy running-config startup-config",
       "Copy configuration\n"
       "Copy running config to... \n"
       "Copy running config to startup config (same as write file)\n")
{
  if (!host.noconfig)
    vty_write_config (vty);
  return CMD_SUCCESS;
}
/** -- **/

/* Write startup configuration into the terminal. */
DEFUN (show_startup_config,
       show_startup_config_cmd,
       "show startup-config",
       SHOW_STR
       "Contents of startup configuration\n")
{
  char buf[BUFSIZ];
  FILE *confp;

  if (host.noconfig)
    return CMD_SUCCESS;
  if (host.config == NULL)
    return CMD_WARNING;

  confp = fopen (host.config, "r");
  if (confp == NULL)
    {
      vty_out (vty, "Can't open configuration file [%s] due to '%s'%s",
               host.config, safe_strerror(errno), VTY_NEWLINE);
      return CMD_WARNING;
    }

  while (fgets (buf, BUFSIZ, confp))
    {
      char *cp = buf;

      while (*cp != '\r' && *cp != '\n' && *cp != '\0')
        cp++;
      *cp = '\0';

      vty_out (vty, "%s%s", buf, VTY_NEWLINE);
    }

  fclose (confp);

  return CMD_SUCCESS;
}

int
cmd_hostname_set (const char *hostname)
{
  XFREE (MTYPE_HOST, host.name);
  host.name = hostname ? XSTRDUP (MTYPE_HOST, hostname) : NULL;
  return CMD_SUCCESS;
}

/* Hostname configuration */
DEFUN (config_hostname,
       hostname_cmd,
       "hostname WORD",
       "Set system's network name\n"
       "This system's network name\n")
{
  struct cmd_token *word = argv[1];

  if (!isalpha((int) word->arg[0]))
    {
      vty_out (vty, "Please specify string starting with alphabet%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  return cmd_hostname_set (word->arg);
}

DEFUN (config_no_hostname,
       no_hostname_cmd,
       "no hostname [HOSTNAME]",
       NO_STR
       "Reset system's network name\n"
       "Host name of this router\n")
{
  return cmd_hostname_set (NULL);
}

/* VTY interface password set. */
DEFUN (config_password,
       password_cmd,
       "password [(8-8)] WORD",
       "Assign the terminal connection password\n"
       "Specifies a HIDDEN password will follow\n"
       "The password string\n")
{
  int idx_8 = 1;
  int idx_word = 2;
  if (argc == 3) // '8' was specified
  {
    if (host.password)
      XFREE (MTYPE_HOST, host.password);
    host.password = NULL;
    if (host.password_encrypt)
      XFREE (MTYPE_HOST, host.password_encrypt);
    host.password_encrypt = XSTRDUP (MTYPE_HOST, argv[idx_word]->arg);
    return CMD_SUCCESS;
  }

  if (!isalnum (argv[idx_8]->arg[0]))
    {
      vty_out (vty,
               "Please specify string starting with alphanumeric%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (host.password)
    XFREE (MTYPE_HOST, host.password);
  host.password = NULL;

  if (host.encrypt)
    {
      if (host.password_encrypt)
        XFREE (MTYPE_HOST, host.password_encrypt);
      host.password_encrypt = XSTRDUP (MTYPE_HOST, zencrypt (argv[idx_8]->arg));
    }
  else
    host.password = XSTRDUP (MTYPE_HOST, argv[idx_8]->arg);

  return CMD_SUCCESS;
}

/* VTY enable password set. */
DEFUN (config_enable_password,
       enable_password_cmd,
       "enable password [(8-8)] WORD",
       "Modify enable password parameters\n"
       "Assign the privileged level password\n"
       "Specifies a HIDDEN password will follow\n"
       "The HIDDEN 'enable' password string\n")
{
  int idx_8 = 2;
  int idx_word = 3;

  /* Crypt type is specified. */
  if (argc == 4)
    {
      if (argv[idx_8]->arg[0] == '8')
        {
          if (host.enable)
            XFREE (MTYPE_HOST, host.enable);
          host.enable = NULL;

          if (host.enable_encrypt)
            XFREE (MTYPE_HOST, host.enable_encrypt);
          host.enable_encrypt = XSTRDUP (MTYPE_HOST, argv[idx_word]->arg);

          return CMD_SUCCESS;
        }
      else
        {
          vty_out (vty, "Unknown encryption type.%s", VTY_NEWLINE);
          return CMD_WARNING;
        }
    }

  if (!isalnum (argv[idx_8]->arg[0]))
    {
      vty_out (vty,
               "Please specify string starting with alphanumeric%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  if (host.enable)
    XFREE (MTYPE_HOST, host.enable);
  host.enable = NULL;

  /* Plain password input. */
  if (host.encrypt)
    {
      if (host.enable_encrypt)
        XFREE (MTYPE_HOST, host.enable_encrypt);
      host.enable_encrypt = XSTRDUP (MTYPE_HOST, zencrypt (argv[idx_8]->arg));
    }
  else
    host.enable = XSTRDUP (MTYPE_HOST, argv[idx_8]->arg);

  return CMD_SUCCESS;
}

/* VTY enable password delete. */
DEFUN (no_config_enable_password,
       no_enable_password_cmd,
       "no enable password",
       NO_STR
       "Modify enable password parameters\n"
       "Assign the privileged level password\n")
{
  if (host.enable)
    XFREE (MTYPE_HOST, host.enable);
  host.enable = NULL;

  if (host.enable_encrypt)
    XFREE (MTYPE_HOST, host.enable_encrypt);
  host.enable_encrypt = NULL;

  return CMD_SUCCESS;
}

DEFUN (service_password_encrypt,
       service_password_encrypt_cmd,
       "service password-encryption",
       "Set up miscellaneous service\n"
       "Enable encrypted passwords\n")
{
  if (host.encrypt)
    return CMD_SUCCESS;

  host.encrypt = 1;

  if (host.password)
    {
      if (host.password_encrypt)
        XFREE (MTYPE_HOST, host.password_encrypt);
      host.password_encrypt = XSTRDUP (MTYPE_HOST, zencrypt (host.password));
    }
  if (host.enable)
    {
      if (host.enable_encrypt)
        XFREE (MTYPE_HOST, host.enable_encrypt);
      host.enable_encrypt = XSTRDUP (MTYPE_HOST, zencrypt (host.enable));
    }

  return CMD_SUCCESS;
}

DEFUN (no_service_password_encrypt,
       no_service_password_encrypt_cmd,
       "no service password-encryption",
       NO_STR
       "Set up miscellaneous service\n"
       "Enable encrypted passwords\n")
{
  if (! host.encrypt)
    return CMD_SUCCESS;

  host.encrypt = 0;

  if (host.password_encrypt)
    XFREE (MTYPE_HOST, host.password_encrypt);
  host.password_encrypt = NULL;

  if (host.enable_encrypt)
    XFREE (MTYPE_HOST, host.enable_encrypt);
  host.enable_encrypt = NULL;

  return CMD_SUCCESS;
}

DEFUN (config_terminal_length,
       config_terminal_length_cmd,
       "terminal length (0-512)",
       "Set terminal line parameters\n"
       "Set number of lines on a screen\n"
       "Number of lines on screen (0 for no pausing)\n")
{
  int idx_number = 2;

  vty->lines = atoi (argv[idx_number]->arg);

  return CMD_SUCCESS;
}

DEFUN (config_terminal_no_length,
       config_terminal_no_length_cmd,
       "terminal no length",
       "Set terminal line parameters\n"
       NO_STR
       "Set number of lines on a screen\n")
{
  vty->lines = -1;
  return CMD_SUCCESS;
}

DEFUN (service_terminal_length,
       service_terminal_length_cmd,
       "service terminal-length (0-512)",
       "Set up miscellaneous service\n"
       "System wide terminal length configuration\n"
       "Number of lines of VTY (0 means no line control)\n")
{
  int idx_number = 2;

  host.lines = atoi (argv[idx_number]->arg);

  return CMD_SUCCESS;
}

DEFUN (no_service_terminal_length,
       no_service_terminal_length_cmd,
       "no service terminal-length [(0-512)]",
       NO_STR
       "Set up miscellaneous service\n"
       "System wide terminal length configuration\n"
       "Number of lines of VTY (0 means no line control)\n")
{
  host.lines = -1;
  return CMD_SUCCESS;
}

DEFUN_HIDDEN (do_echo,
              echo_cmd,
              "echo MESSAGE...",
              "Echo a message back to the vty\n"
              "The message to echo\n")
{
  char *message;

  vty_out (vty, "%s%s", ((message = argv_concat (argv, argc, 1)) ? message : ""),
           VTY_NEWLINE);
  if (message)
    XFREE(MTYPE_TMP, message);
  return CMD_SUCCESS;
}

DEFUN (config_logmsg,
       config_logmsg_cmd,
       "logmsg <emergencies|alerts|critical|errors|warnings|notifications|informational|debugging> MESSAGE...",
       "Send a message to enabled logging destinations\n"
       LOG_LEVEL_DESC
       "The message to send\n")
{
  int idx_log_level = 1;
  int idx_message = 2;
  int level;
  char *message;

  if ((level = level_match(argv[idx_log_level]->arg)) == ZLOG_DISABLED)
    return CMD_ERR_NO_MATCH;

  zlog(level, "%s", ((message = argv_concat(argv, argc, idx_message)) ? message : ""));
  if (message)
    XFREE(MTYPE_TMP, message);

  return CMD_SUCCESS;
}

DEFUN (show_logging,
       show_logging_cmd,
       "show logging",
       SHOW_STR
       "Show current logging configuration\n")
{
  struct zlog *zl = zlog_default;

  vty_out (vty, "Syslog logging: ");
  if (zl->maxlvl[ZLOG_DEST_SYSLOG] == ZLOG_DISABLED)
    vty_out (vty, "disabled");
  else
    vty_out (vty, "level %s, facility %s, ident %s",
             zlog_priority[zl->maxlvl[ZLOG_DEST_SYSLOG]],
             facility_name(zl->facility), zl->ident);
  vty_out (vty, "%s", VTY_NEWLINE);

  vty_out (vty, "Stdout logging: ");
  if (zl->maxlvl[ZLOG_DEST_STDOUT] == ZLOG_DISABLED)
    vty_out (vty, "disabled");
  else
    vty_out (vty, "level %s",
             zlog_priority[zl->maxlvl[ZLOG_DEST_STDOUT]]);
  vty_out (vty, "%s", VTY_NEWLINE);

  vty_out (vty, "Monitor logging: ");
  if (zl->maxlvl[ZLOG_DEST_MONITOR] == ZLOG_DISABLED)
    vty_out (vty, "disabled");
  else
    vty_out (vty, "level %s",
             zlog_priority[zl->maxlvl[ZLOG_DEST_MONITOR]]);
  vty_out (vty, "%s", VTY_NEWLINE);

  vty_out (vty, "File logging: ");
  if ((zl->maxlvl[ZLOG_DEST_FILE] == ZLOG_DISABLED) ||
      !zl->fp)
    vty_out (vty, "disabled");
  else
    vty_out (vty, "level %s, filename %s",
             zlog_priority[zl->maxlvl[ZLOG_DEST_FILE]],
             zl->filename);
  vty_out (vty, "%s", VTY_NEWLINE);

  vty_out (vty, "Protocol name: %s%s",
           zl->protoname, VTY_NEWLINE);
  vty_out (vty, "Record priority: %s%s",
           (zl->record_priority ? "enabled" : "disabled"), VTY_NEWLINE);
  vty_out (vty, "Timestamp precision: %d%s",
           zl->timestamp_precision, VTY_NEWLINE);

  return CMD_SUCCESS;
}

DEFUN (config_log_stdout,
       config_log_stdout_cmd,
       "log stdout [<emergencies|alerts|critical|errors|warnings|notifications|informational|debugging>]",
       "Logging control\n"
       "Set stdout logging level\n"
       LOG_LEVEL_DESC)
{
  int idx_log_level = 2;

  if (argc == idx_log_level)
  {
    zlog_set_level (ZLOG_DEST_STDOUT, zlog_default->default_lvl);
    return CMD_SUCCESS;
  }
  int level;

  if ((level = level_match(argv[idx_log_level]->arg)) == ZLOG_DISABLED)
    return CMD_ERR_NO_MATCH;
  zlog_set_level (ZLOG_DEST_STDOUT, level);
  return CMD_SUCCESS;
}

DEFUN (no_config_log_stdout,
       no_config_log_stdout_cmd,
       "no log stdout [<emergencies|alerts|critical|errors|warnings|notifications|informational|debugging>]",
       NO_STR
       "Logging control\n"
       "Cancel logging to stdout\n"
       LOG_LEVEL_DESC)
{
  zlog_set_level (ZLOG_DEST_STDOUT, ZLOG_DISABLED);
  return CMD_SUCCESS;
}

DEFUN (config_log_monitor,
       config_log_monitor_cmd,
       "log monitor [<emergencies|alerts|critical|errors|warnings|notifications|informational|debugging>]",
       "Logging control\n"
       "Set terminal line (monitor) logging level\n"
       LOG_LEVEL_DESC)
{
  int idx_log_level = 2;

  if (argc == idx_log_level)
  {
    zlog_set_level (ZLOG_DEST_MONITOR, zlog_default->default_lvl);
    return CMD_SUCCESS;
  }
  int level;

  if ((level = level_match(argv[idx_log_level]->arg)) == ZLOG_DISABLED)
    return CMD_ERR_NO_MATCH;
  zlog_set_level (ZLOG_DEST_MONITOR, level);
  return CMD_SUCCESS;
}

DEFUN (no_config_log_monitor,
       no_config_log_monitor_cmd,
       "no log monitor [<emergencies|alerts|critical|errors|warnings|notifications|informational|debugging>]",
       NO_STR
       "Logging control\n"
       "Disable terminal line (monitor) logging\n"
       LOG_LEVEL_DESC)
{
  zlog_set_level (ZLOG_DEST_MONITOR, ZLOG_DISABLED);
  return CMD_SUCCESS;
}

static int
set_log_file(struct vty *vty, const char *fname, int loglevel)
{
  int ret;
  char *p = NULL;
  const char *fullpath;

  /* Path detection. */
  if (! IS_DIRECTORY_SEP (*fname))
    {
      char cwd[MAXPATHLEN+1];
      cwd[MAXPATHLEN] = '\0';

      if (getcwd (cwd, MAXPATHLEN) == NULL)
        {
          zlog_err ("config_log_file: Unable to alloc mem!");
          return CMD_WARNING;
        }

      if ( (p = XMALLOC (MTYPE_TMP, strlen (cwd) + strlen (fname) + 2))
          == NULL)
        {
          zlog_err ("config_log_file: Unable to alloc mem!");
          return CMD_WARNING;
        }
      sprintf (p, "%s/%s", cwd, fname);
      fullpath = p;
    }
  else
    fullpath = fname;

  ret = zlog_set_file (fullpath, loglevel);

  if (p)
    XFREE (MTYPE_TMP, p);

  if (!ret)
    {
      vty_out (vty, "can't open logfile %s\n", fname);
      return CMD_WARNING;
    }

  if (host.logfile)
    XFREE (MTYPE_HOST, host.logfile);

  host.logfile = XSTRDUP (MTYPE_HOST, fname);

#if defined(HAVE_CUMULUS)
  if (zlog_default->maxlvl[ZLOG_DEST_SYSLOG] != ZLOG_DISABLED)
    zlog_default->maxlvl[ZLOG_DEST_SYSLOG] = ZLOG_DISABLED;
#endif
  return CMD_SUCCESS;
}

DEFUN (config_log_file,
       config_log_file_cmd,
       "log file FILENAME [<emergencies|alerts|critical|errors|warnings|notifications|informational|debugging>]",
       "Logging control\n"
       "Logging to file\n"
       "Logging filename\n"
       LOG_LEVEL_DESC)
{
  int idx_filename = 2;
  int idx_log_levels = 3;
  if (argc == 4)
  {
    int level;
    if ((level = level_match(argv[idx_log_levels]->arg)) == ZLOG_DISABLED)
      return CMD_ERR_NO_MATCH;
    return set_log_file(vty, argv[idx_filename]->arg, level);
  }
  else
    return set_log_file(vty, argv[idx_filename]->arg, zlog_default->default_lvl);
}

DEFUN (no_config_log_file,
       no_config_log_file_cmd,
       "no log file [FILENAME [LEVEL]]",
       NO_STR
       "Logging control\n"
       "Cancel logging to file\n"
       "Logging file name\n"
       "Logging file name\n"
       "Logging level\n")
{
  zlog_reset_file ();

  if (host.logfile)
    XFREE (MTYPE_HOST, host.logfile);

  host.logfile = NULL;

  return CMD_SUCCESS;
}

DEFUN (config_log_syslog,
       config_log_syslog_cmd,
       "log syslog [<emergencies|alerts|critical|errors|warnings|notifications|informational|debugging>]",
       "Logging control\n"
       "Set syslog logging level\n"
       LOG_LEVEL_DESC)
{
  int idx_log_levels = 2;
  if (argc == 3)
  {
    int level;
    if ((level = level_match (argv[idx_log_levels]->arg)) == ZLOG_DISABLED)
      return CMD_ERR_NO_MATCH;
    zlog_set_level (ZLOG_DEST_SYSLOG, level);
    return CMD_SUCCESS;
  }
  else
  {
    zlog_set_level (ZLOG_DEST_SYSLOG, zlog_default->default_lvl);
    return CMD_SUCCESS;
  }
}

DEFUN (no_config_log_syslog,
       no_config_log_syslog_cmd,
       "no log syslog [<kern|user|mail|daemon|auth|syslog|lpr|news|uucp|cron|local0|local1|local2|local3|local4|local5|local6|local7>] [<emergencies|alerts|critical|errors|warnings|notifications|informational|debugging>]",
       NO_STR
       "Logging control\n"
       "Cancel logging to syslog\n"
       LOG_FACILITY_DESC
       LOG_LEVEL_DESC)
{
  zlog_set_level (ZLOG_DEST_SYSLOG, ZLOG_DISABLED);
  return CMD_SUCCESS;
}

DEFUN (config_log_facility,
       config_log_facility_cmd,
       "log facility <kern|user|mail|daemon|auth|syslog|lpr|news|uucp|cron|local0|local1|local2|local3|local4|local5|local6|local7>",
       "Logging control\n"
       "Facility parameter for syslog messages\n"
       LOG_FACILITY_DESC)
{
  int idx_target = 2;
  int facility = facility_match(argv[idx_target]->arg);

  zlog_default->facility = facility;
  return CMD_SUCCESS;
}

DEFUN (no_config_log_facility,
       no_config_log_facility_cmd,
       "no log facility [<kern|user|mail|daemon|auth|syslog|lpr|news|uucp|cron|local0|local1|local2|local3|local4|local5|local6|local7>]",
       NO_STR
       "Logging control\n"
       "Reset syslog facility to default (daemon)\n"
       LOG_FACILITY_DESC)
{
  zlog_default->facility = LOG_DAEMON;
  return CMD_SUCCESS;
}

DEFUN_DEPRECATED (config_log_trap,
                  config_log_trap_cmd,
                  "log trap <emergencies|alerts|critical|errors|warnings|notifications|informational|debugging>",
                  "Logging control\n"
                  "(Deprecated) Set logging level and default for all destinations\n"
                  LOG_LEVEL_DESC)
{
  int new_level ;
  int i;

  if ((new_level = level_match(argv[2]->arg)) == ZLOG_DISABLED)
    return CMD_ERR_NO_MATCH;

  zlog_default->default_lvl = new_level;
  for (i = 0; i < ZLOG_NUM_DESTS; i++)
    if (zlog_default->maxlvl[i] != ZLOG_DISABLED)
      zlog_default->maxlvl[i] = new_level;
  return CMD_SUCCESS;
}

DEFUN_DEPRECATED (no_config_log_trap,
                  no_config_log_trap_cmd,
                  "no log trap [emergencies|alerts|critical|errors|warnings|notifications|informational|debugging]",
                  NO_STR
                  "Logging control\n"
                  "Permit all logging information\n"
                  LOG_LEVEL_DESC)
{
  zlog_default->default_lvl = LOG_DEBUG;
  return CMD_SUCCESS;
}

DEFUN (config_log_record_priority,
       config_log_record_priority_cmd,
       "log record-priority",
       "Logging control\n"
       "Log the priority of the message within the message\n")
{
  zlog_default->record_priority = 1 ;
  return CMD_SUCCESS;
}

DEFUN (no_config_log_record_priority,
       no_config_log_record_priority_cmd,
       "no log record-priority",
       NO_STR
       "Logging control\n"
       "Do not log the priority of the message within the message\n")
{
  zlog_default->record_priority = 0 ;
  return CMD_SUCCESS;
}

DEFUN (config_log_timestamp_precision,
       config_log_timestamp_precision_cmd,
       "log timestamp precision (0-6)",
       "Logging control\n"
       "Timestamp configuration\n"
       "Set the timestamp precision\n"
       "Number of subsecond digits\n")
{
  int idx_number = 3;
  VTY_GET_INTEGER_RANGE("Timestamp Precision",
                        zlog_default->timestamp_precision, argv[idx_number]->arg, 0, 6);
  return CMD_SUCCESS;
}

DEFUN (no_config_log_timestamp_precision,
       no_config_log_timestamp_precision_cmd,
       "no log timestamp precision",
       NO_STR
       "Logging control\n"
       "Timestamp configuration\n"
       "Reset the timestamp precision to the default value of 0\n")
{
  zlog_default->timestamp_precision = 0 ;
  return CMD_SUCCESS;
}

int
cmd_banner_motd_file (const char *file)
{
  int success = CMD_SUCCESS;
  char p[PATH_MAX];
  char *rpath;
  char *in;

  rpath = realpath (file, p);
  if (!rpath)
    return CMD_ERR_NO_FILE;
  in = strstr (rpath, SYSCONFDIR);
  if (in == rpath)
    {
      if (host.motdfile)
        XFREE (MTYPE_HOST, host.motdfile);
      host.motdfile = XSTRDUP (MTYPE_HOST, file);
    }
  else
    success = CMD_WARNING;

  return success;
}

DEFUN (banner_motd_file,
       banner_motd_file_cmd,
       "banner motd file FILE",
       "Set banner\n"
       "Banner for motd\n"
       "Banner from a file\n"
       "Filename\n")
{
  int idx_file = 3;
  const char *filename = argv[idx_file]->arg;
  int cmd = cmd_banner_motd_file (filename);

  if (cmd == CMD_ERR_NO_FILE)
    vty_out (vty, "%s does not exist", filename);
  else if (cmd == CMD_WARNING)
    vty_out (vty, "%s must be in %s", filename, SYSCONFDIR);

  return cmd;
}

DEFUN (banner_motd_default,
       banner_motd_default_cmd,
       "banner motd default",
       "Set banner string\n"
       "Strings for motd\n"
       "Default string\n")
{
  host.motd = default_motd;
  return CMD_SUCCESS;
}

DEFUN (no_banner_motd,
       no_banner_motd_cmd,
       "no banner motd",
       NO_STR
       "Set banner string\n"
       "Strings for motd\n")
{
  host.motd = NULL;
  if (host.motdfile)
    XFREE (MTYPE_HOST, host.motdfile);
  host.motdfile = NULL;
  return CMD_SUCCESS;
}

/* Set config filename.  Called from vty.c */
void
host_config_set (const char *filename)
{
  if (host.config)
    XFREE (MTYPE_HOST, host.config);
  host.config = XSTRDUP (MTYPE_HOST, filename);
}

const char *
host_config_get (void)
{
  return host.config;
}

void
install_default (enum node_type node)
{
  install_element (node, &config_exit_cmd);
  install_element (node, &config_quit_cmd);
  install_element (node, &config_end_cmd);
  install_element (node, &config_help_cmd);
  install_element (node, &config_list_cmd);

  install_element (node, &config_write_cmd);
  install_element (node, &show_running_config_cmd);

  install_element (node, &autocomplete_cmd);
}

/* Initialize command interface. Install basic nodes and commands.
 *
 * terminal = 0 -- vtysh / no logging, no config control
 * terminal = 1 -- normal daemon
 * terminal = -1 -- watchfrr / no logging, but minimal config control */
void
cmd_init (int terminal)
{
  qobj_init ();

  varhandlers = list_new ();

  /* Allocate initial top vector of commands. */
  cmdvec = vector_init (VECTOR_MIN_SIZE);

  /* Default host value settings. */
  host.name = NULL;
  host.password = NULL;
  host.enable = NULL;
  host.logfile = NULL;
  host.config = NULL;
  host.noconfig = (terminal < 0);
  host.lines = -1;
  host.motd = default_motd;
  host.motdfile = NULL;

  /* Install top nodes. */
  install_node (&view_node, NULL);
  install_node (&enable_node, NULL);
  install_node (&auth_node, NULL);
  install_node (&auth_enable_node, NULL);
  install_node (&config_node, config_write_host);

  /* Each node's basic commands. */
  install_element (VIEW_NODE, &show_version_cmd);
  if (terminal)
    {
      install_element (VIEW_NODE, &config_list_cmd);
      install_element (VIEW_NODE, &config_exit_cmd);
      install_element (VIEW_NODE, &config_quit_cmd);
      install_element (VIEW_NODE, &config_help_cmd);
      install_element (VIEW_NODE, &config_enable_cmd);
      install_element (VIEW_NODE, &config_terminal_length_cmd);
      install_element (VIEW_NODE, &config_terminal_no_length_cmd);
      install_element (VIEW_NODE, &show_logging_cmd);
      install_element (VIEW_NODE, &show_commandtree_cmd);
      install_element (VIEW_NODE, &echo_cmd);
      install_element (VIEW_NODE, &autocomplete_cmd);
    }

  if (terminal)
    {
      install_element (ENABLE_NODE, &config_end_cmd);
      install_element (ENABLE_NODE, &config_disable_cmd);
      install_element (ENABLE_NODE, &config_terminal_cmd);
      install_element (ENABLE_NODE, &copy_runningconf_startupconf_cmd);
      install_element (ENABLE_NODE, &config_write_cmd);
      install_element (ENABLE_NODE, &show_running_config_cmd);
    }
  install_element (ENABLE_NODE, &show_startup_config_cmd);

  if (terminal)
    {
      install_element (ENABLE_NODE, &config_logmsg_cmd);
      install_default (CONFIG_NODE);

      thread_cmd_init ();
      workqueue_cmd_init ();
    }

  install_element (CONFIG_NODE, &hostname_cmd);
  install_element (CONFIG_NODE, &no_hostname_cmd);
  install_element (CONFIG_NODE, &frr_version_defaults_cmd);

  if (terminal > 0)
    {
      install_element (CONFIG_NODE, &password_cmd);
      install_element (CONFIG_NODE, &enable_password_cmd);
      install_element (CONFIG_NODE, &no_enable_password_cmd);

      install_element (CONFIG_NODE, &config_log_stdout_cmd);
      install_element (CONFIG_NODE, &no_config_log_stdout_cmd);
      install_element (CONFIG_NODE, &config_log_monitor_cmd);
      install_element (CONFIG_NODE, &no_config_log_monitor_cmd);
      install_element (CONFIG_NODE, &config_log_file_cmd);
      install_element (CONFIG_NODE, &no_config_log_file_cmd);
      install_element (CONFIG_NODE, &config_log_syslog_cmd);
      install_element (CONFIG_NODE, &no_config_log_syslog_cmd);
      install_element (CONFIG_NODE, &config_log_facility_cmd);
      install_element (CONFIG_NODE, &no_config_log_facility_cmd);
      install_element (CONFIG_NODE, &config_log_trap_cmd);
      install_element (CONFIG_NODE, &no_config_log_trap_cmd);
      install_element (CONFIG_NODE, &config_log_record_priority_cmd);
      install_element (CONFIG_NODE, &no_config_log_record_priority_cmd);
      install_element (CONFIG_NODE, &config_log_timestamp_precision_cmd);
      install_element (CONFIG_NODE, &no_config_log_timestamp_precision_cmd);
      install_element (CONFIG_NODE, &service_password_encrypt_cmd);
      install_element (CONFIG_NODE, &no_service_password_encrypt_cmd);
      install_element (CONFIG_NODE, &banner_motd_default_cmd);
      install_element (CONFIG_NODE, &banner_motd_file_cmd);
      install_element (CONFIG_NODE, &no_banner_motd_cmd);
      install_element (CONFIG_NODE, &service_terminal_length_cmd);
      install_element (CONFIG_NODE, &no_service_terminal_length_cmd);

      vrf_install_commands ();
    }

#ifdef DEV_BUILD
  grammar_sandbox_init();
#endif
}

void
cmd_terminate ()
{
  struct cmd_node *cmd_node;

  if (cmdvec)
    {
      for (unsigned int i = 0; i < vector_active (cmdvec); i++)
        if ((cmd_node = vector_slot (cmdvec, i)) != NULL)
        {
          // deleting the graph delets the cmd_element as well
          graph_delete_graph (cmd_node->cmdgraph);
          vector_free (cmd_node->cmd_vector);
          hash_clean (cmd_node->cmd_hash, NULL);
          hash_free (cmd_node->cmd_hash);
          cmd_node->cmd_hash = NULL;
        }

      vector_free (cmdvec);
      cmdvec = NULL;
    }

  if (host.name)
    XFREE (MTYPE_HOST, host.name);
  if (host.password)
    XFREE (MTYPE_HOST, host.password);
  if (host.password_encrypt)
    XFREE (MTYPE_HOST, host.password_encrypt);
  if (host.enable)
    XFREE (MTYPE_HOST, host.enable);
  if (host.enable_encrypt)
    XFREE (MTYPE_HOST, host.enable_encrypt);
  if (host.logfile)
    XFREE (MTYPE_HOST, host.logfile);
  if (host.motdfile)
    XFREE (MTYPE_HOST, host.motdfile);
  if (host.config)
    XFREE (MTYPE_HOST, host.config);

  qobj_finish ();
}
