/* 
   Copyright (C) 2002, 2003, 2004, 2005, 2006, 2007 Bastien Nocera
   Copyright (C) 2003, 2004 Colin Walters <walters@rhythmbox.org>

   The Gnome Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Gnome Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Gnome Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301  USA.

   Author: Bastien Nocera <hadess@hadess.net>
 */

#include "config.h"

#ifndef TOTEM_PL_PARSER_MINI
#include <string.h>
#include <glib.h>
#include <glib/gi18n-lib.h>
#include <libxml/tree.h>
#include <libxml/parser.h>
#include <gtk/gtk.h>
#include "totem-pl-parser.h"
#include "totemplparser-marshal.h"
#endif /* !TOTEM_PL_PARSER_MINI */

#include "totem-pl-parser-mini.h"
#include "totem-pl-parser-xspf.h"
#include "totem-pl-parser-private.h"

#ifndef TOTEM_PL_PARSER_MINI

#define SAFE_FREE(x) { if (x != NULL) xmlFree (x); }

static xmlDocPtr
totem_pl_parser_parse_xml_file (GFile *file)
{
	xmlDocPtr doc;
	char *contents;
	gsize size;

	if (g_file_load_contents (file, NULL, &contents, &size, NULL, NULL) == FALSE)
		return NULL;

	/* Try to remove HTML style comments */
	{
		char *needle;

		while ((needle = strstr (contents, "<!--")) != NULL) {
			while (strncmp (needle, "-->", 3) != 0) {
				*needle = ' ';
				needle++;
				if (*needle == '\0')
					break;
			}
		}
	}

	doc = xmlParseMemory (contents, size);
	if (doc == NULL)
		doc = xmlRecoverMemory (contents, size);
	g_free (contents);

	return doc;
}

gboolean
totem_pl_parser_write_xspf (TotemPlParser *parser, GtkTreeModel *model,
			   TotemPlParserIterFunc func, 
			   GFile *output, const char *title,
			   gpointer user_data, GError **error)
{
	GFileOutputStream *stream;
	int num_entries_total, num_entries, i;
	char *buf;
	gboolean success;

	num_entries = totem_pl_parser_num_entries (parser, model, func, user_data);
	num_entries_total = gtk_tree_model_iter_n_children (model, NULL);

	stream = g_file_replace (output, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
	if (stream == NULL)
		return FALSE;

	buf = g_strdup_printf ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
				"<playlist version=\"1\" xmlns=\"http://xspf.org/ns/0/\">\n"
				" <trackList>\n");
	success = totem_pl_parser_write_string (G_OUTPUT_STREAM (stream), buf, error);
	g_free (buf);
	if (success == FALSE)
		return FALSE;

	for (i = 1; i <= num_entries_total; i++) {
		GtkTreeIter iter;
		char *uri, *uri_escaped, *relative, *title;
		GFile *file;
		gboolean custom_title;

		if (gtk_tree_model_iter_nth_child (model, &iter, NULL, i - 1) == FALSE)
			continue;

		func (model, &iter, &uri, &title, &custom_title, user_data);
		file = g_file_new_for_uri (uri);

		if (totem_pl_parser_scheme_is_ignored (parser, file) != FALSE) {
			g_object_unref (file);
			g_free (uri);
			g_free (title);
			continue;
		}
		g_object_unref (file);

		relative = totem_pl_parser_relative (output, uri);
		uri_escaped = g_markup_escape_text (relative ? relative : uri, -1);
		buf = g_strdup_printf ("  <track>\n"
					"   <location>%s</location>\n", uri_escaped);
		success = totem_pl_parser_write_string (G_OUTPUT_STREAM (stream), buf, error);
		g_free (uri);
		g_free (uri_escaped);
		g_free (relative);
		g_free (buf);
		if (success == FALSE) {
			g_free (title);
			return FALSE;
		}

		if (custom_title == TRUE)
			buf = g_strdup_printf ("   <title>%s</title>\n"
						"  </track>\n", title);
		else
			buf = g_strdup_printf ("  </track>\n");
		
		success = totem_pl_parser_write_string (G_OUTPUT_STREAM (stream), buf, error);
		g_free (buf);
		g_free (title);
		if (success == FALSE)
			return FALSE;
	}

	buf = g_strdup_printf (" </trackList>\n"
				"</playlist>");
	success = totem_pl_parser_write_string (G_OUTPUT_STREAM (stream), buf, error);
	g_free (buf);

	g_object_unref (stream);

	return success;
}

static gboolean
parse_xspf_track (TotemPlParser *parser, GFile *base_file, xmlDocPtr doc,
		xmlNodePtr parent)
{
	xmlNodePtr node;
	xmlChar *title, *uri, *image_uri, *artist, *album, *duration, *moreinfo;
	xmlChar *download_uri, *id;
	GFile *resolved;
	char *resolved_uri;
	TotemPlParserResult retval = TOTEM_PL_PARSER_RESULT_ERROR;
	
	title = NULL;
	uri = NULL;
	image_uri = NULL;
	artist = NULL;
	album = NULL;
	duration = NULL;
	moreinfo = NULL;
	download_uri = NULL;
	id = NULL;

	for (node = parent->children; node != NULL; node = node->next)
	{
		if (node->name == NULL)
			continue;

		if (g_ascii_strcasecmp ((char *)node->name, "location") == 0)
			uri = xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
		else if (g_ascii_strcasecmp ((char *)node->name, "title") == 0)
			title = xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
		else if (g_ascii_strcasecmp ((char *)node->name, "image") == 0)
			image_uri = xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
		/* Last.fm uses creator for the artist */
		else if (g_ascii_strcasecmp ((char *)node->name, "creator") == 0)
			artist = xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
		else if (g_ascii_strcasecmp ((char *)node->name, "duration") == 0)
			duration = xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
		else if (g_ascii_strcasecmp ((char *)node->name, "link") == 0) {
			xmlChar *rel;

			rel = xmlGetProp (node, (const xmlChar *) "rel");
			if (rel != NULL) {
				if (g_ascii_strcasecmp ((char *) rel, "http://www.last.fm/trackpage") == 0)
					moreinfo = xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
				else if (g_ascii_strcasecmp ((char *) rel, "http://www.last.fm/freeTrackURL") == 0)
					download_uri = xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
				xmlFree (rel);
			} else {
				/* If we don't have a rel="", then it's not a last.fm playlist */
				moreinfo = xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
			}
		} else if (g_ascii_strcasecmp ((char *)node->name, "album") == 0)
			album = xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
		else if (g_ascii_strcasecmp ((char *)node->name, "trackauth") == 0)
			id = xmlNodeListGetString (doc, node->xmlChildrenNode, 1);
	}

	if (uri == NULL) {
		retval = TOTEM_PL_PARSER_RESULT_ERROR;
		goto bail;
	}

	resolved_uri = totem_pl_parser_resolve_uri (base_file, (char *) uri);
	resolved = g_file_new_for_uri (resolved_uri);
	g_free (resolved_uri);

	totem_pl_parser_add_uri (parser,
				 TOTEM_PL_PARSER_FIELD_FILE, resolved,
				 TOTEM_PL_PARSER_FIELD_TITLE, title,
				 TOTEM_PL_PARSER_FIELD_DURATION_MS, duration,
				 TOTEM_PL_PARSER_FIELD_IMAGE_URI, image_uri,
				 TOTEM_PL_PARSER_FIELD_AUTHOR, artist,
				 TOTEM_PL_PARSER_FIELD_ALBUM, album,
				 TOTEM_PL_PARSER_FIELD_MOREINFO, moreinfo,
				 TOTEM_PL_PARSER_FIELD_DOWNLOAD_URI, download_uri,
				 TOTEM_PL_PARSER_FIELD_ID, id,
				 NULL);
	g_object_unref (resolved);

	retval = TOTEM_PL_PARSER_RESULT_SUCCESS;

bail:
	SAFE_FREE (title);
	SAFE_FREE (uri);
	SAFE_FREE (image_uri);
	SAFE_FREE (artist);
	SAFE_FREE (album);
	SAFE_FREE (duration);
	SAFE_FREE (moreinfo);
	SAFE_FREE (download_uri);
	SAFE_FREE (id);

	return retval;
}

static gboolean
parse_xspf_trackList (TotemPlParser *parser, GFile *base_file, xmlDocPtr doc,
		xmlNodePtr parent)
{
	xmlNodePtr node;
	TotemPlParserResult retval = TOTEM_PL_PARSER_RESULT_ERROR;

	for (node = parent->children; node != NULL; node = node->next)
	{
		if (node->name == NULL)
			continue;

		if (g_ascii_strcasecmp ((char *)node->name, "track") == 0)
			if (parse_xspf_track (parser, base_file, doc, node) != FALSE)
				retval = TOTEM_PL_PARSER_RESULT_SUCCESS;
	}

	return retval;
}

static gboolean
parse_xspf_entries (TotemPlParser *parser, GFile *base_file, xmlDocPtr doc,
		xmlNodePtr parent)
{
	xmlNodePtr node;
	TotemPlParserResult retval = TOTEM_PL_PARSER_RESULT_ERROR;

	for (node = parent->children; node != NULL; node = node->next) {
		if (node->name == NULL)
			continue;

		if (g_ascii_strcasecmp ((char *)node->name, "trackList") == 0)
			if (parse_xspf_trackList (parser, base_file, doc, node) != FALSE)
				retval = TOTEM_PL_PARSER_RESULT_SUCCESS;
	}

	return retval;
}

TotemPlParserResult
totem_pl_parser_add_xspf (TotemPlParser *parser,
			  GFile *file,
			  GFile *base_file,
			  gpointer data)
{
	xmlDocPtr doc;
	xmlNodePtr node;
	TotemPlParserResult retval = TOTEM_PL_PARSER_RESULT_UNHANDLED;

	doc = totem_pl_parser_parse_xml_file (file);

	/* If the document has no root, or no name */
	if(!doc || !doc->children
			|| !doc->children->name
			|| g_ascii_strcasecmp ((char *)doc->children->name,
				"playlist") != 0) {
		if (doc != NULL)
			xmlFreeDoc(doc);
		return TOTEM_PL_PARSER_RESULT_ERROR;
	}

	for (node = doc->children; node != NULL; node = node->next) {
		if (parse_xspf_entries (parser, base_file, doc, node) != FALSE)
			retval = TOTEM_PL_PARSER_RESULT_SUCCESS;
	}

	xmlFreeDoc(doc);
	return retval;
}
#endif /* !TOTEM_PL_PARSER_MINI */

