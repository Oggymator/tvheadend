/*
 *  tvheadend, channel functions
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include <libhts/htscfg.h>
#include <libhts/htssettings.h>

#include "tvhead.h"
#include "v4l.h"
#include "iptv_input.h"
#include "psi.h"
#include "channels.h"
#include "transports.h"
#include "epg.h"
#include "pvr.h"
#include "autorec.h"

struct channel_tree channel_tree;

static int
dictcmp(const char *a, const char *b)
{
  long int da, db;

  while(1) {
    switch((*a >= '0' && *a <= '9' ? 1 : 0)|(*b >= '0' && *b <= '9' ? 2 : 0)) {
    case 0:  /* 0: a is not a digit, nor is b */
      if(*a != *b)
	return *(const unsigned char *)a - *(const unsigned char *)b;
      if(*a == 0)
	return 0;
      a++;
      b++;
      break;
    case 1:  /* 1: a is a digit,  b is not */
    case 2:  /* 2: a is not a digit,  b is */
	return *(const unsigned char *)a - *(const unsigned char *)b;
    case 3:  /* both are digits, switch to integer compare */
      da = strtol(a, (char **)&a, 10);
      db = strtol(b, (char **)&b, 10);
      if(da != db)
	return da - db;
      break;
    }
  }
}


/**
 *
 */
static int
channelcmp(const channel_t *a, const channel_t *b)
{
  return dictcmp(a->ch_name, b->ch_name);
}


/**
 *
 */
static void
channel_set_name(channel_t *ch, const char *name)
{
  channel_t *x;
  const char *n2;
  int l, i;
  char *cp, c;

  free((void *)ch->ch_name);
  free((void *)ch->ch_sname);

  ch->ch_name = strdup(name);

  l = strlen(name);
  ch->ch_sname = cp = malloc(l + 1);

  n2 = utf8toprintable(name);

  for(i = 0; i < strlen(n2); i++) {
    c = tolower(n2[i]);
    if(isalnum(c))
      *cp++ = c;
    else
      *cp++ = '-';
  }
  *cp = 0;

  free((void *)n2);

  x = RB_INSERT_SORTED(&channel_tree, ch, ch_global_link, channelcmp);
  assert(x == NULL);
}

/**
 *
 */
channel_t *
channel_find(const char *name, int create)
{
  channel_t *ch, skel;

  skel.ch_name = name;

  if((ch = RB_FIND(&channel_tree, &skel,  ch_global_link, channelcmp)) != NULL)
    return ch;

  if(create == 0)
    return NULL;

  ch = calloc(1, sizeof(channel_t));
  ch->ch_index = channel_tree.entries;

  TAILQ_INIT(&ch->ch_epg_events);

  channel_set_name(ch, name);

  ch->ch_tag = tag_get();
  return ch;
}


static struct strtab commercial_detect_tab[] = {
  { "none",       COMMERCIAL_DETECT_NONE   },
  { "ttp192",     COMMERCIAL_DETECT_TTP192 },
};


/**
 *
 */
void
channels_load(void)
{
#if 0
  struct config_head cl;
  config_entry_t *ce;
  char buf[PATH_MAX];
  DIR *dir;
  struct dirent *d;
  const char *name, *grp, *x;
  channel_t *ch;
  int v;

  TAILQ_INIT(&all_channel_groups);
  TAILQ_INIT(&cl);

  snprintf(buf, sizeof(buf), "%s/channel-group-settings.cfg", settings_dir);
  config_read_file0(buf, &cl);

  TAILQ_FOREACH(ce, &cl, ce_link) {
    if(ce->ce_type != CFG_SUB || strcasecmp("channel-group", ce->ce_key))
      continue;
     
    if((name = config_get_str_sub(&ce->ce_sub, "name", NULL)) == NULL)
      continue;

    channel_group_find(name, 1);
  }
  config_free0(&cl);

  tcg = channel_group_find("-disabled-", 1);
  tcg->tcg_cant_delete_me = 1;
  tcg->tcg_hidden = 1;
  
  defgroup = channel_group_find("Uncategorized", 1);
  defgroup->tcg_cant_delete_me = 1;

  snprintf(buf, sizeof(buf), "%s/channels", settings_dir);

  if((dir = opendir(buf)) == NULL)
    return;
  
  while((d = readdir(dir)) != NULL) {

    if(d->d_name[0] == '.')
      continue;

    snprintf(buf, sizeof(buf), "%s/channels/%s", settings_dir, d->d_name);
    TAILQ_INIT(&cl);
    config_read_file0(buf, &cl);

    name = config_get_str_sub(&cl, "name", NULL);
    grp = config_get_str_sub(&cl, "channel-group", NULL);
    if(name != NULL && grp != NULL) {
      tcg = channel_group_find(grp, 1);
      ch = channel_find(name, 1, tcg);
    
      x = config_get_str_sub(&cl, "commercial-detect", NULL);
      if(x != NULL) {
	v = str2val(x, commercial_detect_tab);
	if(v > 1)
	  ch->ch_commercial_detection = v;
      }

      if((x = config_get_str_sub(&cl, "icon", NULL)) != NULL)
	ch->ch_icon = strdup(x);
    }
    config_free0(&cl);
  }

  closedir(dir);

  
  /* Static services */

  TAILQ_FOREACH(ce, &config_list, ce_link) {
    if(ce->ce_type == CFG_SUB && !strcasecmp("service", ce->ce_key)) {
      service_load(&ce->ce_sub);
    }
  }
#endif
}



/**
 * The index stuff should go away
 */
channel_t *
channel_by_index(uint32_t index)
{
  channel_t *ch;

  RB_FOREACH(ch, &channel_tree, ch_global_link)
    if(ch->ch_index == index)
      return ch;

  return NULL;
}



/**
 *
 */
channel_t *
channel_by_tag(uint32_t tag)
{
  channel_t *ch;

  RB_FOREACH(ch, &channel_tree, ch_global_link)
    if(ch->ch_tag == tag)
      return ch;

  return NULL;
}




/**
 * Write out a config file for a channel
 */
static void
channel_save(channel_t *ch)
{
  htsmsg_t *m = htsmsg_create();
  htsmsg_add_str(m, "icon", ch->ch_icon);
  htsmsg_add_str(m, "commercial_detect", 
		 val2str(ch->ch_commercial_detection,
			 commercial_detect_tab) ?: "?");
  hts_settings_save(m, "channels/%s", ch->ch_name);
  htsmsg_destroy(m);
}

/**
 * Rename a channel and all tied transports
 */
int
channel_rename(channel_t *ch, const char *newname)
{
  th_transport_t *t;

  if(channel_find(newname, 0))
    return -1;

  hts_settings_remove("channels/%s", ch->ch_name);

  RB_REMOVE(&channel_tree, ch, ch_global_link);
  channel_set_name(ch, newname);

  LIST_FOREACH(t, &ch->ch_transports, tht_ch_link) {
    free(t->tht_chname);
    t->tht_chname = strdup(newname);
    t->tht_config_change(t);
  }

  channel_save(ch);
  return 0;
}

/**
 * Delete channel
 */
void
channel_delete(channel_t *ch)
{
  th_transport_t *t;
  th_subscription_t *s;

  pvr_destroy_by_channel(ch);

  while((t = LIST_FIRST(&ch->ch_transports)) != NULL) {
    transport_unmap_channel(t);
    t->tht_config_change(t);
  }

  while((s = LIST_FIRST(&ch->ch_subscriptions)) != NULL) {
    LIST_REMOVE(s, ths_channel_link);
    s->ths_channel = NULL;
  }

  epg_destroy_by_channel(ch);

  autorec_destroy_by_channel(ch);

  hts_settings_remove("channels/%s", ch->ch_name);

  free((void *)ch->ch_name);
  free((void *)ch->ch_sname);
  free(ch->ch_icon);
  
  RB_REMOVE(&channel_tree, ch, ch_global_link);
  free(ch);
}



/**
 * Merge transports from channel 'src' to channel 'dst'
 *
 * Then, destroy the 'src' channel
 */
void
channel_merge(channel_t *dst, channel_t *src)
{
  th_transport_t *t;

  while((t = LIST_FIRST(&src->ch_transports)) != NULL) {
    transport_unmap_channel(t);

    transport_map_channel(t, dst);
    t->tht_config_change(t);
  }

  channel_delete(src);
}

/**
 *
 */
void
channel_set_icon(channel_t *ch, const char *icon)
{
  free(ch->ch_icon);
  ch->ch_icon = icon ? strdup(icon) : NULL;
  channel_save(ch);
}
