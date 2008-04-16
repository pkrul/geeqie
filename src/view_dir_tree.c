/*
 * Geeqie
 * (C) 2006 John Ellis
 *
 * Author: John Ellis
 *
 * This software is released under the GNU General Public License (GNU GPL).
 * Please read the included file COPYING for more information.
 * This software comes with no warranty of any kind, use at your own risk!
 */

#include "main.h"
#include "view_dir_tree.h"


#include "dnd.h"
#include "dupe.h"
#include "filelist.h"
#include "layout.h"
#include "layout_image.h"
#include "layout_util.h"
#include "utilops.h"
#include "ui_bookmark.h"
#include "ui_fileops.h"
#include "ui_menu.h"
#include "ui_tree_edit.h"
#include "view_dir.h"

#include <gdk/gdkkeysyms.h> /* for keyboard values */


#define VDTREE_INDENT 14
#define VDTREE_PAD 4

#define VDTREE_INFO(_vd_, _part_) (((ViewDirInfoTree *)(_vd_->info))->_part_)


typedef struct _PathData PathData;
struct _PathData
{
	gchar *name;
	FileData *node;
};

typedef struct _NodeData NodeData;
struct _NodeData
{
	FileData *fd;
	gint expanded;
	time_t last_update;
};


static gint vdtree_populate_path_by_iter(ViewDir *vd, GtkTreeIter *iter, gint force, const gchar *target_path);
static FileData *vdtree_populate_path(ViewDir *vd, const gchar *path, gint expand, gint force);


/*
 *----------------------------------------------------------------------------
 * utils
 *----------------------------------------------------------------------------
 */

static void set_cursor(GtkWidget *widget, GdkCursorType cursor_type)
{
	GdkCursor *cursor = NULL;

	if (!widget || !widget->window) return;

	if (cursor_type > -1) cursor = gdk_cursor_new (cursor_type);
	gdk_window_set_cursor (widget->window, cursor);
	if (cursor) gdk_cursor_unref(cursor);
	gdk_flush();
}

static void vdtree_busy_push(ViewDir *vd)
{
	if (VDTREE_INFO(vd, busy_ref) == 0) set_cursor(vd->view, GDK_WATCH);
	VDTREE_INFO(vd, busy_ref)++;
}

static void vdtree_busy_pop(ViewDir *vd)
{
	if (VDTREE_INFO(vd, busy_ref) == 1) set_cursor(vd->view, -1);
	if (VDTREE_INFO(vd, busy_ref) > 0) VDTREE_INFO(vd, busy_ref)--;
}

gint vdtree_find_row(ViewDir *vd, FileData *fd, GtkTreeIter *iter, GtkTreeIter *parent)
{
	GtkTreeModel *store;
	gint valid;

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view));
	if (parent)
		{
		valid = gtk_tree_model_iter_children(store, iter, parent);
		}
	else
		{
		valid = gtk_tree_model_get_iter_first(store, iter);
		}
	while (valid)
		{
		NodeData *nd;
		GtkTreeIter found;

		gtk_tree_model_get(GTK_TREE_MODEL(store), iter, DIR_COLUMN_POINTER, &nd, -1);
		if (nd->fd == fd) return TRUE;

		if (vdtree_find_row(vd, fd, &found, iter))
			{
			memcpy(iter, &found, sizeof(found));
			return TRUE;
			}

		valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), iter);
		}

	return FALSE;
}

static void vdtree_icon_set_by_iter(ViewDir *vd, GtkTreeIter *iter, GdkPixbuf *pixbuf)
{
	GtkTreeModel *store;
	GdkPixbuf *old;

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view));
	gtk_tree_model_get(store, iter, DIR_COLUMN_ICON, &old, -1);
	if (old != vd->pf->deny)
		{
		gtk_tree_store_set(GTK_TREE_STORE(store), iter, DIR_COLUMN_ICON, pixbuf, -1);
		}
}

static void vdtree_expand_by_iter(ViewDir *vd, GtkTreeIter *iter, gint expand)
{
	GtkTreeModel *store;
	GtkTreePath *tpath;

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view));
	tpath = gtk_tree_model_get_path(store, iter);
	if (expand)
		{
		gtk_tree_view_expand_row(GTK_TREE_VIEW(vd->view), tpath, FALSE);
		vdtree_icon_set_by_iter(vd, iter, vd->pf->open);
		}
	else
		{
		gtk_tree_view_collapse_row(GTK_TREE_VIEW(vd->view), tpath);
		}
	gtk_tree_path_free(tpath);
}

static void vdtree_expand_by_data(ViewDir *vd, FileData *fd, gint expand)
{
	GtkTreeIter iter;

	if (vdtree_find_row(vd, fd, &iter, NULL))
		{
		vdtree_expand_by_iter(vd, &iter, expand);
		}
}

static gint vdtree_rename_row_cb(TreeEditData *td, const gchar *old, const gchar *new, gpointer data)
{
	ViewDir *vd = data;
	GtkTreeModel *store;
	GtkTreeIter iter;
	NodeData *nd;
	gchar *old_path;
	gchar *new_path;
	gchar *base;

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view));
	if (!gtk_tree_model_get_iter(store, &iter, td->path)) return FALSE;
	gtk_tree_model_get(store, &iter, DIR_COLUMN_POINTER, &nd, -1);
	if (!nd) return FALSE;

	old_path = g_strdup(nd->fd->path);

	base = remove_level_from_path(old_path);
	new_path = concat_dir_and_file(base, new);
	g_free(base);

	if (file_util_rename_dir(nd->fd, new_path, vd->view))
		{
		vdtree_populate_path(vd, new_path, TRUE, TRUE);

		if (vd->layout && strcmp(vd->path, old_path) == 0)
			{
			layout_set_path(vd->layout, new_path);
			}
		}

	g_free(old_path);
	g_free(new_path);

	return FALSE;
}

static void vdtree_rename_by_data(ViewDir *vd, FileData *fd)
{
	GtkTreeModel *store;
	GtkTreePath *tpath;
	GtkTreeIter iter;

	if (!fd ||
	    !vdtree_find_row(vd, fd, &iter, NULL)) return;

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view));
	tpath = gtk_tree_model_get_path(store, &iter);

	tree_edit_by_path(GTK_TREE_VIEW(vd->view), tpath, 0, fd->name,
			  vdtree_rename_row_cb, vd);
	gtk_tree_path_free(tpath);
}

static void vdtree_node_free(NodeData *nd)
{
	if (!nd) return;

	file_data_unref(nd->fd);
	g_free(nd);
}

/*
 *-----------------------------------------------------------------------------
 * pop-up menu
 *-----------------------------------------------------------------------------
 */

static void vdtree_pop_menu_up_cb(GtkWidget *widget, gpointer data)
{
	ViewDir *vd = data;
	gchar *path;

	if (!vd->path || strcmp(vd->path, "/") == 0) return;
	path = remove_level_from_path(vd->path);

	if (vd->select_func)
		{
		vd->select_func(vd, path, vd->select_data);
		}

	g_free(path);
}

static void vdtree_pop_menu_slide_cb(GtkWidget *widget, gpointer data)
{
	ViewDir *vd = data;
	gchar *path;

	if (!vd->layout) return;

	if (!vd->click_fd) return;
	path = vd->click_fd->path;

	layout_set_path(vd->layout, path);
	layout_select_none(vd->layout);
	layout_image_slideshow_stop(vd->layout);
	layout_image_slideshow_start(vd->layout);
}

static void vdtree_pop_menu_slide_rec_cb(GtkWidget *widget, gpointer data)
{
	ViewDir *vd = data;
	gchar *path;
	GList *list;

	if (!vd->layout) return;

	if (!vd->click_fd) return;
	path = vd->click_fd->path;

	list = filelist_recursive(path);

	layout_image_slideshow_stop(vd->layout);
	layout_image_slideshow_start_from_list(vd->layout, list);
}

static void vdtree_pop_menu_dupe(ViewDir *vd, gint recursive)
{
	DupeWindow *dw;
	GList *list = NULL;

	if (!vd->click_fd) return;

	if (recursive)
		{
		list = g_list_append(list, file_data_ref(vd->click_fd));
		}
	else
		{
		filelist_read(vd->click_fd->path, &list, NULL);
		list = filelist_filter(list, FALSE);
		}

	dw = dupe_window_new(DUPE_MATCH_NAME);
	dupe_window_add_files(dw, list, recursive);

	filelist_free(list);
}

static void vdtree_pop_menu_dupe_cb(GtkWidget *widget, gpointer data)
{
	ViewDir *vd = data;
	vdtree_pop_menu_dupe(vd, FALSE);
}

static void vdtree_pop_menu_dupe_rec_cb(GtkWidget *widget, gpointer data)
{
	ViewDir *vd = data;
	vdtree_pop_menu_dupe(vd, TRUE);
}

static void vdtree_pop_menu_new_cb(GtkWidget *widget, gpointer data)
{
	ViewDir *vd = data;
	const gchar *path;
	gchar *new_path;
	gchar *buf;

	if (!vd->click_fd) return;
	path = vd->click_fd->path;

	buf = concat_dir_and_file(path, _("new_folder"));
	new_path = unique_filename(buf, NULL, NULL, FALSE);
	g_free(buf);
	if (!new_path) return;

	if (!mkdir_utf8(new_path, 0755))
		{
		gchar *text;

		text = g_strdup_printf(_("Unable to create folder:\n%s"), new_path);
		file_util_warning_dialog(_("Error creating folder"), text, GTK_STOCK_DIALOG_ERROR, vd->view);
		g_free(text);
		}
	else
		{
		FileData *fd;

		fd = vdtree_populate_path(vd, new_path, TRUE, TRUE);

		vdtree_rename_by_data(vd, fd);
		}

	g_free(new_path);
}

static void vdtree_pop_menu_rename_cb(GtkWidget *widget, gpointer data)
{
	ViewDir *vd = data;

	vdtree_rename_by_data(vd, vd->click_fd);
}

static void vdtree_pop_menu_delete_cb(GtkWidget *widget, gpointer data)
{
	ViewDir *vd = data;

	if (!vd->click_fd) return;
	file_util_delete_dir(vd->click_fd, vd->widget);
}

static void vdtree_pop_menu_dir_view_as_cb(GtkWidget *widget, gpointer data)
{
	ViewDir *vd = data;

	if (vd->layout) layout_views_set(vd->layout, DIRVIEW_LIST, vd->layout->icon_view);
}

static void vdtree_pop_menu_refresh_cb(GtkWidget *widget, gpointer data)
{
	ViewDir *vd = data;

	if (vd->layout) layout_refresh(vd->layout);
}

static void vdtree_toggle_show_hidden_files_cb(GtkWidget *widget, gpointer data)
{
	ViewDir *vd = data;

	options->file_filter.show_hidden_files = !options->file_filter.show_hidden_files;
	if (vd->layout) layout_refresh(vd->layout);
}

static GtkWidget *vdtree_pop_menu(ViewDir *vd, FileData *fd)
{
	GtkWidget *menu;
	gint active;
	gint parent_active = FALSE;

	active = (fd != NULL);
	if (fd)
		{
		gchar *parent;

		parent = remove_level_from_path(fd->path);
		parent_active = access_file(parent, W_OK | X_OK);
		g_free(parent);
		}

	menu = popup_menu_short_lived();
	g_signal_connect(G_OBJECT(menu), "destroy",
			 G_CALLBACK(vd_popup_destroy_cb), vd);

	menu_item_add_stock_sensitive(menu, _("_Up to parent"), GTK_STOCK_GO_UP,
		       		      (vd->path && strcmp(vd->path, "/") != 0),
				      G_CALLBACK(vdtree_pop_menu_up_cb), vd);

	menu_item_add_divider(menu);
	menu_item_add_sensitive(menu, _("_Slideshow"), active,
				G_CALLBACK(vdtree_pop_menu_slide_cb), vd);
	menu_item_add_sensitive(menu, _("Slideshow recursive"), active,
				G_CALLBACK(vdtree_pop_menu_slide_rec_cb), vd);

	menu_item_add_divider(menu);
	menu_item_add_stock_sensitive(menu, _("Find _duplicates..."), GTK_STOCK_FIND, active,
				      G_CALLBACK(vdtree_pop_menu_dupe_cb), vd);
	menu_item_add_stock_sensitive(menu, _("Find duplicates recursive..."), GTK_STOCK_FIND, active,
				      G_CALLBACK(vdtree_pop_menu_dupe_rec_cb), vd);

	menu_item_add_divider(menu);

	active = (fd &&
		  access_file(fd->path, W_OK | X_OK));
	menu_item_add_sensitive(menu, _("_New folder..."), active,
				G_CALLBACK(vdtree_pop_menu_new_cb), vd);

	menu_item_add_sensitive(menu, _("_Rename..."), parent_active,
				G_CALLBACK(vdtree_pop_menu_rename_cb), vd);
	menu_item_add_stock_sensitive(menu, _("_Delete..."), GTK_STOCK_DELETE, parent_active,
				      G_CALLBACK(vdtree_pop_menu_delete_cb), vd);

	menu_item_add_divider(menu);
	menu_item_add_check(menu, _("View as _tree"), TRUE,
			    G_CALLBACK(vdtree_pop_menu_dir_view_as_cb), vd);
	menu_item_add_check(menu, _("Show _hidden files"), options->file_filter.show_hidden_files,
			    G_CALLBACK(vdtree_toggle_show_hidden_files_cb), vd);

	menu_item_add_stock(menu, _("Re_fresh"), GTK_STOCK_REFRESH,
			    G_CALLBACK(vdtree_pop_menu_refresh_cb), vd);

	return menu;
}

/*
 *----------------------------------------------------------------------------
 * dnd
 *----------------------------------------------------------------------------
 */

static GtkTargetEntry vdtree_dnd_drop_types[] = {
	{ "text/uri-list", 0, TARGET_URI_LIST }
};
static gint vdtree_dnd_drop_types_count = 1;


static void vdtree_dest_set(ViewDir *vd, gint enable)
{
	if (enable)
		{
		gtk_drag_dest_set(vd->view,
				  GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_DROP,
				  vdtree_dnd_drop_types, vdtree_dnd_drop_types_count,
				  GDK_ACTION_MOVE | GDK_ACTION_COPY);
		}
	else
		{
		gtk_drag_dest_unset(vd->view);
		}
}

static void vdtree_dnd_get(GtkWidget *widget, GdkDragContext *context,
			   GtkSelectionData *selection_data, guint info,
			   guint time, gpointer data)
{
	ViewDir *vd = data;
	GList *list;
	gchar *uri_text = NULL;
	gint length = 0;

	if (!vd->click_fd) return;

	switch (info)
		{
		case TARGET_URI_LIST:
		case TARGET_TEXT_PLAIN:
			list = g_list_prepend(NULL, vd->click_fd);
			uri_text = uri_text_from_filelist(list, &length, (info == TARGET_TEXT_PLAIN));
			g_list_free(list);
			break;
		}

	if (uri_text)
		{
		gtk_selection_data_set(selection_data, selection_data->target,
				       8, (guchar *)uri_text, length);
		g_free(uri_text);
		}
}

static void vdtree_dnd_begin(GtkWidget *widget, GdkDragContext *context, gpointer data)
{
	ViewDir *vd = data;

	vd_color_set(vd, vd->click_fd, TRUE);
	vdtree_dest_set(vd, FALSE);
}

static void vdtree_dnd_end(GtkWidget *widget, GdkDragContext *context, gpointer data)
{
	ViewDir *vd = data;

	vd_color_set(vd, vd->click_fd, FALSE);
	vdtree_dest_set(vd, TRUE);
}

static void vdtree_dnd_drop_receive(GtkWidget *widget,
				    GdkDragContext *context, gint x, gint y,
				    GtkSelectionData *selection_data, guint info,
				    guint time, gpointer data)
{
	ViewDir *vd = data;
	GtkTreePath *tpath;
	GtkTreeIter iter;
	FileData *fd = NULL;

	vd->click_fd = NULL;

	if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), x, y,
					  &tpath, NULL, NULL, NULL))
		{
		GtkTreeModel *store;
		NodeData *nd;

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, DIR_COLUMN_POINTER, &nd, -1);
		gtk_tree_path_free(tpath);

		fd = (nd) ? nd->fd : NULL;
		}

	if (!fd) return;

        if (info == TARGET_URI_LIST)
                {
		GList *list;
		gint active;

		list = uri_filelist_from_text((gchar *)selection_data->data, TRUE);
		if (!list) return;

		active = access_file(fd->path, W_OK | X_OK);

		vd_color_set(vd, fd, TRUE);
		vd->popup = vd_drop_menu(vd, active);
		gtk_menu_popup(GTK_MENU(vd->popup), NULL, NULL, NULL, NULL, 0, time);

		vd->drop_fd = fd;
		vd->drop_list = list;
		}
}

static gint vdtree_dnd_drop_expand_cb(gpointer data)
{
	ViewDir *vd = data;
	GtkTreeIter iter;

	if (vd->drop_fd &&
	    vdtree_find_row(vd, vd->drop_fd, &iter, NULL))
		{
		vdtree_populate_path_by_iter(vd, &iter, FALSE, vd->path);
		vdtree_expand_by_data(vd, vd->drop_fd, TRUE);
		}

	VDTREE_INFO(vd, drop_expand_id) = -1;
	return FALSE;
}

static void vdtree_dnd_drop_expand_cancel(ViewDir *vd)
{
	if (VDTREE_INFO(vd, drop_expand_id) != -1) g_source_remove(VDTREE_INFO(vd, drop_expand_id));
	VDTREE_INFO(vd, drop_expand_id) = -1;
}

static void vdtree_dnd_drop_expand(ViewDir *vd)
{
	vdtree_dnd_drop_expand_cancel(vd);
	VDTREE_INFO(vd, drop_expand_id) = g_timeout_add(1000, vdtree_dnd_drop_expand_cb, vd);
}

static void vdtree_drop_update(ViewDir *vd, gint x, gint y)
{
	GtkTreePath *tpath;
	GtkTreeIter iter;
	FileData *fd = NULL;

	if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(vd->view), x, y,
					  &tpath, NULL, NULL, NULL))
		{
		GtkTreeModel *store;
		NodeData *nd;

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view));
		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, DIR_COLUMN_POINTER, &nd, -1);
		gtk_tree_path_free(tpath);

		fd = (nd) ? nd->fd : NULL;
		}

	if (fd != vd->drop_fd)
		{
		vd_color_set(vd, vd->drop_fd, FALSE);
		vd_color_set(vd, fd, TRUE);
		if (fd) vdtree_dnd_drop_expand(vd);
		}

	vd->drop_fd = fd;
}

static void vdtree_dnd_drop_scroll_cancel(ViewDir *vd)
{
	if (vd->drop_scroll_id != -1) g_source_remove(vd->drop_scroll_id);
	vd->drop_scroll_id = -1;
}

static gint vdtree_auto_scroll_idle_cb(gpointer data)
{
	ViewDir *vd = data;

	if (vd->drop_fd)
		{
		GdkWindow *window;
		gint x, y;
		gint w, h;

		window = vd->view->window;
		gdk_window_get_pointer(window, &x, &y, NULL);
		gdk_drawable_get_size(window, &w, &h);
		if (x >= 0 && x < w && y >= 0 && y < h)
			{
			vdtree_drop_update(vd, x, y);
			}
		}

	vd->drop_scroll_id = -1;
	return FALSE;
}

static gint vdtree_auto_scroll_notify_cb(GtkWidget *widget, gint x, gint y, gpointer data)
{
	ViewDir *vd = data;

	if (!vd->drop_fd || vd->drop_list) return FALSE;

	if (vd->drop_scroll_id == -1) vd->drop_scroll_id = g_idle_add(vdtree_auto_scroll_idle_cb, vd);

	return TRUE;
}

static gint vdtree_dnd_drop_motion(GtkWidget *widget, GdkDragContext *context,
				   gint x, gint y, guint time, gpointer data)
{
        ViewDir *vd = data;

	vd->click_fd = NULL;

	if (gtk_drag_get_source_widget(context) == vd->view)
		{
		gdk_drag_status(context, 0, time);
		return TRUE;
		}
	else
		{
		gdk_drag_status(context, context->suggested_action, time);
		}

	vdtree_drop_update(vd, x, y);

	if (vd->drop_fd)
		{
		GtkAdjustment *adj = gtk_tree_view_get_vadjustment(GTK_TREE_VIEW(vd->view));
		widget_auto_scroll_start(vd->view, adj, -1, -1, vdtree_auto_scroll_notify_cb, vd);
		}

	return FALSE;
}

static void vdtree_dnd_drop_leave(GtkWidget *widget, GdkDragContext *context, guint time, gpointer data)
{
	ViewDir *vd = data;

	if (vd->drop_fd != vd->click_fd) vd_color_set(vd, vd->drop_fd, FALSE);

	vd->drop_fd = NULL;

	vdtree_dnd_drop_expand_cancel(vd);
}

static void vdtree_dnd_init(ViewDir *vd)
{
	gtk_drag_source_set(vd->view, GDK_BUTTON1_MASK | GDK_BUTTON2_MASK,
			    dnd_file_drag_types, dnd_file_drag_types_count,
			    GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_ASK);
	g_signal_connect(G_OBJECT(vd->view), "drag_data_get",
			 G_CALLBACK(vdtree_dnd_get), vd);
	g_signal_connect(G_OBJECT(vd->view), "drag_begin",
			 G_CALLBACK(vdtree_dnd_begin), vd);
	g_signal_connect(G_OBJECT(vd->view), "drag_end",
			 G_CALLBACK(vdtree_dnd_end), vd);

	vdtree_dest_set(vd, TRUE);
	g_signal_connect(G_OBJECT(vd->view), "drag_data_received",
			 G_CALLBACK(vdtree_dnd_drop_receive), vd);
	g_signal_connect(G_OBJECT(vd->view), "drag_motion",
			 G_CALLBACK(vdtree_dnd_drop_motion), vd);
	g_signal_connect(G_OBJECT(vd->view), "drag_leave",
			 G_CALLBACK(vdtree_dnd_drop_leave), vd);
}

/*
 *----------------------------------------------------------------------------
 * parts lists
 *----------------------------------------------------------------------------
 */

static GList *parts_list(const gchar *path)
{
	GList *list = NULL;
	const gchar *strb, *strp;
	gint l;

	strp = path;

	if (*strp != '/') return NULL;

	strp++;
	strb = strp;
	l = 0;

	while (*strp != '\0')
		{
		if (*strp == '/')
			{
			if (l > 0) list = g_list_prepend(list, g_strndup(strb, l));
			strp++;
			strb = strp;
			l = 0;
			}
		else
			{
			strp++;
			l++;
			}
		}
	if (l > 0) list = g_list_prepend(list, g_strndup(strb, l));

	list = g_list_reverse(list);

	list = g_list_prepend(list, g_strdup("/"));

	return list;
}

static void parts_list_free(GList *list)
{
	GList *work = list;
	while (work)
		{
		PathData *pd = work->data;
		g_free(pd->name);
		g_free(pd);
		work = work->next;
		}

	g_list_free(list);
}

static GList *parts_list_add_node_points(ViewDir *vd, GList *list)
{
	GList *work;
	GtkTreeModel *store;
	GtkTreeIter iter;
	gint valid;

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view));
	valid = gtk_tree_model_get_iter_first(store, &iter);

	work = list;
	while (work)
		{
		PathData *pd;
		FileData *fd = NULL;

		pd = g_new0(PathData, 1);
		pd->name = work->data;

		while (valid && !fd)
			{
			NodeData *nd;

			gtk_tree_model_get(store, &iter, DIR_COLUMN_POINTER, &nd, -1);
			if (strcmp(nd->fd->name, pd->name) == 0)
				{
				fd = nd->fd;
				}
			else
				{
				valid = gtk_tree_model_iter_next(store, &iter);
				}
			}

		pd->node = fd;
		work->data = pd;

		if (fd)
			{
			GtkTreeIter parent;
			memcpy(&parent, &iter, sizeof(parent));
			valid = gtk_tree_model_iter_children(store, &iter, &parent);
			}

		work = work->next;
		}

	return list;
}

/*
 *----------------------------------------------------------------------------
 * misc
 *----------------------------------------------------------------------------
 */

#if 0
static void vdtree_row_deleted_cb(GtkTreeModel *tree_model, GtkTreePath *tpath, gpointer data)
{
	GtkTreeIter iter;
	NodeData *nd;

	gtk_tree_model_get_iter(tree_model, &iter, tpath);
	gtk_tree_model_get(tree_model, &iter, DIR_COLUMN_POINTER, &nd, -1);

	if (!nd) return;

	file_data_unref(nd->fd);
	g_free(nd);
}
#endif

/*
 *----------------------------------------------------------------------------
 * node traversal, management
 *----------------------------------------------------------------------------
 */

static gint vdtree_find_iter_by_data(ViewDir *vd, GtkTreeIter *parent, NodeData *nd, GtkTreeIter *iter)
{
	GtkTreeModel *store;

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view));
	if (!nd || !gtk_tree_model_iter_children(store, iter, parent)) return -1;
	do	{
		NodeData *cnd;

		gtk_tree_model_get(store, iter, DIR_COLUMN_POINTER, &cnd, -1);
		if (cnd == nd) return TRUE;
		} while (gtk_tree_model_iter_next(store, iter));

	return FALSE;
}

static NodeData *vdtree_find_iter_by_name(ViewDir *vd, GtkTreeIter *parent, const gchar *name, GtkTreeIter *iter)
{
	GtkTreeModel *store;

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view));
	if (!name || !gtk_tree_model_iter_children(store, iter, parent)) return NULL;
	do	{
		NodeData *nd;

		gtk_tree_model_get(store, iter, DIR_COLUMN_POINTER, &nd, -1);
		if (nd && strcmp(nd->fd->name, name) == 0) return nd;
		} while (gtk_tree_model_iter_next(store, iter));

	return NULL;
}

static void vdtree_add_by_data(ViewDir *vd, FileData *fd, GtkTreeIter *parent)
{
	GtkTreeStore *store;
	GtkTreeIter child;
	NodeData *nd;
	GdkPixbuf *pixbuf;
	NodeData *end;
	GtkTreeIter empty;

	if (!fd) return;

	if (access_file(fd->path, R_OK | X_OK))
		{
		pixbuf = vd->pf->close;
		}
	else
		{
		pixbuf = vd->pf->deny;
		}

	nd = g_new0(NodeData, 1);
	nd->fd = fd;
	nd->expanded = FALSE;
	nd->last_update = time(NULL);

	store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view)));
	gtk_tree_store_append(store, &child, parent);
	gtk_tree_store_set(store, &child, DIR_COLUMN_POINTER, nd,
					 DIR_COLUMN_ICON, pixbuf,
					 DIR_COLUMN_NAME, nd->fd->name,
					 DIR_COLUMN_COLOR, FALSE, -1);

	/* all nodes are created with an "empty" node, so that the expander is shown
	 * this is removed when the child is populated */
	end = g_new0(NodeData, 1);
	end->fd = file_data_new_simple("");
	end->expanded = TRUE;

	gtk_tree_store_append(store, &empty, &child);
	gtk_tree_store_set(store, &empty, DIR_COLUMN_POINTER, end,
					  DIR_COLUMN_NAME, "empty", -1);

	if (parent)
		{
		NodeData *pnd;
		GtkTreePath *tpath;

		gtk_tree_model_get(GTK_TREE_MODEL(store), parent, DIR_COLUMN_POINTER, &pnd, -1);
		tpath = gtk_tree_model_get_path(GTK_TREE_MODEL(store), parent);
		if (options->tree_descend_subdirs &&
		    gtk_tree_view_row_expanded(GTK_TREE_VIEW(vd->view), tpath) &&
		    !nd->expanded)
			{
			vdtree_populate_path_by_iter(vd, &child, FALSE, vd->path);
			}
		gtk_tree_path_free(tpath);
		}
}

static gint vdtree_populate_path_by_iter(ViewDir *vd, GtkTreeIter *iter, gint force, const gchar *target_path)
{
	GtkTreeModel *store;
	GList *list;
	GList *work;
	GList *old;
	time_t current_time;
	GtkTreeIter child;
	NodeData *nd;

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view));
	gtk_tree_model_get(store, iter, DIR_COLUMN_POINTER, &nd, -1);

	if (!nd) return FALSE;

	current_time = time(NULL);
	
	if (nd->expanded)
		{
		if (!force && current_time - nd->last_update < 10) return TRUE;
		if (!isdir(nd->fd->path))
			{
			if (vd->click_fd == nd->fd) vd->click_fd = NULL;
			if (vd->drop_fd == nd->fd) vd->drop_fd = NULL;
			gtk_tree_store_remove(GTK_TREE_STORE(store), iter);
			vdtree_node_free(nd);
			return FALSE;
			}
		if (!force && filetime(nd->fd->path) == nd->fd->date) return TRUE;
		}

	vdtree_busy_push(vd);

	list = NULL;
	filelist_read(nd->fd->path, NULL, &list);

	/* when hidden files are not enabled, and the user enters a hidden path,
	 * allow the tree to display that path by specifically inserting the hidden entries
	 */
	if (!options->file_filter.show_hidden_files &&
	    target_path &&
	    strncmp(nd->fd->path, target_path, strlen(nd->fd->path)) == 0)
		{
		gint n;

		n = strlen(nd->fd->path);
		if (target_path[n] == '/' && target_path[n+1] == '.')
			{
			gchar *name8;
			struct stat sbuf;

			n++;

			while (target_path[n] != '\0' && target_path[n] != '/') n++;
			name8 = g_strndup(target_path, n);

			if (stat_utf8(name8, &sbuf))
				{
				list = g_list_prepend(list, file_data_new_simple(name8));
				}

			g_free(name8);
			}
		}

	old = NULL;
	if (gtk_tree_model_iter_children(store, &child, iter))
		{
		do	{
			NodeData *cnd;

			gtk_tree_model_get(store, &child, DIR_COLUMN_POINTER, &cnd, -1);
			old = g_list_prepend(old, cnd);
			} while (gtk_tree_model_iter_next(store, &child));
		}

	work = list;
	while (work)
		{
		FileData *fd;

		fd = work->data;
		work = work->next;

		if (strcmp(fd->name, ".") == 0 || strcmp(fd->name, "..") == 0)
			{
			file_data_unref(fd);
			}
		else
			{
			NodeData *cnd;

			cnd = vdtree_find_iter_by_name(vd, iter, fd->name, &child);
			if (cnd)
				{
				old = g_list_remove(old, cnd);
				if (cnd->expanded && cnd->fd->date != fd->date &&
				    vdtree_populate_path_by_iter(vd, &child, FALSE, target_path))
					{
					cnd->fd->size = fd->size;
					cnd->fd->date = fd->date;
					}

				file_data_unref(fd);
				}
			else
				{
				vdtree_add_by_data(vd, fd, iter);
				}
			}
		}

	work = old;
	while (work)
		{
		NodeData *cnd = work->data;
		work = work->next;

		if (vd->click_fd == cnd->fd) vd->click_fd = NULL;
		if (vd->drop_fd == cnd->fd) vd->drop_fd = NULL;

		if (vdtree_find_iter_by_data(vd, iter, cnd, &child))
			{
			gtk_tree_store_remove(GTK_TREE_STORE(store), &child);
			vdtree_node_free(cnd);
			}
		}

	g_list_free(old);
	g_list_free(list);

	vdtree_busy_pop(vd);

	nd->expanded = TRUE;
	nd->last_update = current_time;

	return TRUE;
}

static FileData *vdtree_populate_path(ViewDir *vd, const gchar *path, gint expand, gint force)
{
	GList *list;
	GList *work;
	FileData *fd = NULL;

	if (!path) return NULL;

	vdtree_busy_push(vd);

	list = parts_list(path);
	list = parts_list_add_node_points(vd, list);

	work = list;
	while (work)
		{
		PathData *pd = work->data;
		if (pd->node == NULL)
			{
			PathData *parent_pd;
			GtkTreeIter parent_iter;
			GtkTreeIter iter;
			NodeData *nd;

			if (work == list)
				{
				/* should not happen */
				printf("vdtree warning, root node not found\n");
				parts_list_free(list);
				vdtree_busy_pop(vd);
				return NULL;
				}

			parent_pd = work->prev->data;

			if (!vdtree_find_row(vd, parent_pd->node, &parent_iter, NULL) ||
			    !vdtree_populate_path_by_iter(vd, &parent_iter, force, path) ||
			    (nd = vdtree_find_iter_by_name(vd, &parent_iter, pd->name, &iter)) == NULL)
				{
				printf("vdtree warning, aborted at %s\n", parent_pd->name);
				parts_list_free(list);
				vdtree_busy_pop(vd);
				return NULL;
				}

			pd->node = nd->fd;

			if (pd->node)
				{
				if (expand)
					{
					vdtree_expand_by_iter(vd, &parent_iter, TRUE);
					vdtree_expand_by_iter(vd, &iter, TRUE);
					}
				vdtree_populate_path_by_iter(vd, &iter, force, path);
				}
			}
		else
			{
			GtkTreeIter iter;

			if (vdtree_find_row(vd, pd->node, &iter, NULL))
				{
				if (expand) vdtree_expand_by_iter(vd, &iter, TRUE);
				vdtree_populate_path_by_iter(vd, &iter, force, path);
				}
			}

		work = work->next;
		}

	work = g_list_last(list);
	if (work)
		{
		PathData *pd = work->data;
		fd = pd->node;
		}
	parts_list_free(list);

	vdtree_busy_pop(vd);

	return fd;
}

/*
 *----------------------------------------------------------------------------
 * access
 *----------------------------------------------------------------------------
 */

static gint selection_is_ok = FALSE;

static gboolean vdtree_select_cb(GtkTreeSelection *selection, GtkTreeModel *store, GtkTreePath *tpath,
                                 gboolean path_currently_selected, gpointer data)
{
	return selection_is_ok;
}

static void vdtree_select_row(ViewDir *vd, FileData *fd)
{
	GtkTreeSelection *selection;
	GtkTreeIter iter;
                                                                                                                               
	if (!vdtree_find_row(vd, fd, &iter, NULL)) return;
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vd->view));

	/* hack, such that selection is only allowed to be changed from here */
	selection_is_ok = TRUE;
	gtk_tree_selection_select_iter(selection, &iter);
	selection_is_ok = FALSE;

	if (!vdtree_populate_path_by_iter(vd, &iter, FALSE, vd->path)) return;

	vdtree_expand_by_iter(vd, &iter, TRUE);

        if (fd && vd->select_func)
                {
                vd->select_func(vd, fd->path, vd->select_data);
                }
}

gint vdtree_set_path(ViewDir *vd, const gchar *path)
{
	FileData *fd;
	GtkTreeIter iter;

	if (!path) return FALSE;
	if (vd->path && strcmp(path, vd->path) == 0) return TRUE;

	g_free(vd->path);
	vd->path = g_strdup(path);

	fd = vdtree_populate_path(vd, vd->path, TRUE, FALSE);

	if (!fd) return FALSE;

	if (vdtree_find_row(vd, fd, &iter, NULL))
		{
		GtkTreeModel *store;
		GtkTreePath *tpath;

		tree_view_row_make_visible(GTK_TREE_VIEW(vd->view), &iter, TRUE);

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view));
		tpath = gtk_tree_model_get_path(store, &iter);
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(vd->view), tpath, NULL, FALSE);
		gtk_tree_path_free(tpath);

		vdtree_select_row(vd, fd);
		}

	return TRUE;
}

#if 0
const gchar *vdtree_get_path(ViewDir *vd)
{
	return vd->path;
}
#endif

void vdtree_refresh(ViewDir *vd)
{
	vdtree_populate_path(vd, vd->path, FALSE, TRUE);
}

const gchar *vdtree_row_get_path(ViewDir *vd, gint row)
{
	printf("FIXME: no get row path\n");
	return NULL;
}

/*
 *----------------------------------------------------------------------------
 * callbacks
 *----------------------------------------------------------------------------
 */

static void vdtree_menu_position_cb(GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
	ViewDir *vd = data;
	GtkTreeModel *store;
	GtkTreeIter iter;
	GtkTreePath *tpath;
	gint cw, ch;

	if (vdtree_find_row(vd, vd->click_fd, &iter, NULL) < 0) return;
	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view));
	tpath = gtk_tree_model_get_path(store, &iter);
	tree_view_get_cell_clamped(GTK_TREE_VIEW(vd->view), tpath, 0, TRUE, x, y, &cw, &ch);
	gtk_tree_path_free(tpath);
	*y += ch;
	popup_menu_position_clamp(menu, x, y, 0);
}

static gint vdtree_press_key_cb(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
	ViewDir *vd = data;
	GtkTreePath *tpath;
	GtkTreeIter iter;
	FileData *fd = NULL;

	gtk_tree_view_get_cursor(GTK_TREE_VIEW(vd->view), &tpath, NULL);
	if (tpath)
		{
		GtkTreeModel *store;
		NodeData *nd;

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, DIR_COLUMN_POINTER, &nd, -1);

		gtk_tree_path_free(tpath);

		fd = (nd) ? nd->fd : NULL;
		}

	switch (event->keyval)
		{
		case GDK_Menu:
			vd->click_fd = fd;
			vd_color_set(vd, vd->click_fd, TRUE);

			vd->popup = vdtree_pop_menu(vd, vd->click_fd);
			gtk_menu_popup(GTK_MENU(vd->popup), NULL, NULL, vdtree_menu_position_cb, vd, 0, GDK_CURRENT_TIME);

			return TRUE;
			break;
		case GDK_plus:
		case GDK_Right:
		case GDK_KP_Add:
			if (fd)
				{
				vdtree_populate_path_by_iter(vd, &iter, FALSE, vd->path);
				vdtree_icon_set_by_iter(vd, &iter, vd->pf->open);
				}
			break;
		}

	return FALSE;
}

static gint vdtree_clicked_on_expander(GtkTreeView *treeview, GtkTreePath *tpath,
				       GtkTreeViewColumn *column, gint x, gint y, gint *left_of_expander)
{
	gint depth;
	gint size;
	gint sep;
	gint exp_width;

	if (column != gtk_tree_view_get_expander_column(treeview)) return FALSE;

	gtk_widget_style_get(GTK_WIDGET(treeview), "expander-size", &size, "horizontal-separator", &sep, NULL);
	depth = gtk_tree_path_get_depth(tpath);

	exp_width = sep + size + sep;

	if (x <= depth * exp_width)
		{
		if (left_of_expander) *left_of_expander = !(x >= (depth - 1) * exp_width);
		return TRUE;
		}

	return FALSE;
}

static gint vdtree_press_cb(GtkWidget *widget, GdkEventButton *bevent, gpointer data)
{
	ViewDir *vd = data;
	GtkTreePath *tpath;
	GtkTreeViewColumn *column;
	GtkTreeIter iter;
	NodeData *nd = NULL;

	if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), bevent->x, bevent->y,
					  &tpath, &column, NULL, NULL))
		{
		GtkTreeModel *store;
		gint left_of_expander;

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, DIR_COLUMN_POINTER, &nd, -1);
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(widget), tpath, NULL, FALSE);

		if (vdtree_clicked_on_expander(GTK_TREE_VIEW(widget), tpath, column, bevent->x, bevent->y, &left_of_expander))
			{
			vd->click_fd = NULL;

			/* clicking this region should automatically reveal an expander, if necessary
			 * treeview bug: the expander will not expand until a button_motion_event highlights it.
			 */
			if (bevent->button == 1 &&
			    !left_of_expander &&
			    !gtk_tree_view_row_expanded(GTK_TREE_VIEW(vd->view), tpath))
				{
				vdtree_populate_path_by_iter(vd, &iter, FALSE, vd->path);
				vdtree_icon_set_by_iter(vd, &iter, vd->pf->open);
				}

			gtk_tree_path_free(tpath);
			return FALSE;
			}

		gtk_tree_path_free(tpath);
		}

	vd->click_fd = (nd) ? nd->fd : NULL;
	vd_color_set(vd, vd->click_fd, TRUE);

	if (bevent->button == 3)
		{
		vd->popup = vdtree_pop_menu(vd, vd->click_fd);
		gtk_menu_popup(GTK_MENU(vd->popup), NULL, NULL, NULL, NULL,
			       bevent->button, bevent->time);
		}

	return (bevent->button != 1);
}

static gint vdtree_release_cb(GtkWidget *widget, GdkEventButton *bevent, gpointer data)
{
	ViewDir *vd = data;
	GtkTreePath *tpath;
	GtkTreeIter iter;
	NodeData *nd = NULL;

	if (!vd->click_fd) return FALSE;
	vd_color_set(vd, vd->click_fd, FALSE);

	if (bevent->button != 1) return TRUE;

	if ((bevent->x != 0 || bevent->y != 0) &&
	    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), bevent->x, bevent->y,
					  &tpath, NULL, NULL, NULL))
		{
		GtkTreeModel *store;

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, DIR_COLUMN_POINTER, &nd, -1);
		gtk_tree_path_free(tpath);
		}

	if (nd && vd->click_fd == nd->fd)
		{
		vdtree_select_row(vd, vd->click_fd);
		}

	return FALSE;
}

static void vdtree_row_expanded(GtkTreeView *treeview, GtkTreeIter *iter, GtkTreePath *tpath, gpointer data)
{
	ViewDir *vd = data;

	vdtree_populate_path_by_iter(vd, iter, FALSE, NULL);
	vdtree_icon_set_by_iter(vd, iter, vd->pf->open);
}

static void vdtree_row_collapsed(GtkTreeView *treeview, GtkTreeIter *iter, GtkTreePath *tpath, gpointer data)
{
	ViewDir *vd = data;

	vdtree_icon_set_by_iter(vd, iter, vd->pf->close);
}

static gint vdtree_sort_cb(GtkTreeModel *store, GtkTreeIter *a, GtkTreeIter *b, gpointer data)
{
	NodeData *nda;
	NodeData *ndb;

	gtk_tree_model_get(store, a, DIR_COLUMN_POINTER, &nda, -1);
	gtk_tree_model_get(store, b, DIR_COLUMN_POINTER, &ndb, -1);

	return CASE_SORT(nda->fd->name, ndb->fd->name);
}

/*
 *----------------------------------------------------------------------------
 * core
 *----------------------------------------------------------------------------
 */

static void vdtree_setup_root(ViewDir *vd)
{
	const gchar *path = "/";
	FileData *fd;


	fd = file_data_new_simple(path);
	vdtree_add_by_data(vd, fd, NULL);

	vdtree_expand_by_data(vd, fd, TRUE);
	vdtree_populate_path(vd, path, FALSE, FALSE);
}

static void vdtree_activate_cb(GtkTreeView *tview, GtkTreePath *tpath, GtkTreeViewColumn *column, gpointer data)
{
	ViewDir *vd = data;
	GtkTreeModel *store;
	GtkTreeIter iter;
	NodeData *nd;

	store = gtk_tree_view_get_model(tview);
	gtk_tree_model_get_iter(store, &iter, tpath);
	gtk_tree_model_get(store, &iter, DIR_COLUMN_POINTER, &nd, -1);

	vdtree_select_row(vd, nd->fd);
}

static GdkColor *vdtree_color_shifted(GtkWidget *widget)
{
	static GdkColor color;
	static GtkWidget *done = NULL;

	if (done != widget)
		{
		GtkStyle *style;

		style = gtk_widget_get_style(widget);
		memcpy(&color, &style->base[GTK_STATE_NORMAL], sizeof(color));
		shift_color(&color, -1, 0);
		done = widget;
		}

	return &color;
}

static void vdtree_color_cb(GtkTreeViewColumn *tree_column, GtkCellRenderer *cell,
			    GtkTreeModel *tree_model, GtkTreeIter *iter, gpointer data)
{
	ViewDir *vd = data;
	gboolean set;

	gtk_tree_model_get(tree_model, iter, DIR_COLUMN_COLOR, &set, -1);
	g_object_set(G_OBJECT(cell),
		     "cell-background-gdk", vdtree_color_shifted(vd->view),
		     "cell-background-set", set, NULL);
}

static gboolean vdtree_destroy_node_cb(GtkTreeModel *store, GtkTreePath *tpath, GtkTreeIter *iter, gpointer data)
{
	NodeData *nd;

	gtk_tree_model_get(store, iter, DIR_COLUMN_POINTER, &nd, -1);
	vdtree_node_free(nd);

	return FALSE;
}

static void vdtree_destroy_cb(GtkWidget *widget, gpointer data)
{
	ViewDir *vd = data;
	GtkTreeModel *store;

	vdtree_dnd_drop_expand_cancel(vd);
	vdtree_dnd_drop_scroll_cancel(vd);
	widget_auto_scroll_stop(vd->view);

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vd->view));
	gtk_tree_model_foreach(store, vdtree_destroy_node_cb, vd);
}

ViewDir *vdtree_new(ViewDir *vd, const gchar *path)
{
	GtkTreeStore *store;
	GtkTreeSelection *selection;
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;

	vd->info = g_new0(ViewDirInfoTree, 1);
	vd->type = DIRVIEW_TREE;
	vd->widget_destroy_cb = vdtree_destroy_cb;

	VDTREE_INFO(vd, drop_expand_id) = -1;

	VDTREE_INFO(vd, busy_ref) = 0;

	store = gtk_tree_store_new(4, G_TYPE_POINTER, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_INT);
	vd->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	g_object_unref(store);

	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(vd->view), FALSE);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(vd->view), FALSE);
	gtk_tree_sortable_set_default_sort_func(GTK_TREE_SORTABLE(store), vdtree_sort_cb, vd, NULL);
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store),
					     GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID, GTK_SORT_ASCENDING);

	g_signal_connect(G_OBJECT(vd->view), "row_activated",
			 G_CALLBACK(vdtree_activate_cb), vd);
	g_signal_connect(G_OBJECT(vd->view), "row_expanded",
			 G_CALLBACK(vdtree_row_expanded), vd);
	g_signal_connect(G_OBJECT(vd->view), "row_collapsed",
			 G_CALLBACK(vdtree_row_collapsed), vd);
#if 0
	g_signal_connect(G_OBJECT(store), "row_deleted",
			 G_CALLBACK(vdtree_row_deleted_cb), vd);
#endif

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vd->view));
	gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
	gtk_tree_selection_set_select_function(selection, vdtree_select_cb, vd, NULL);

	column = gtk_tree_view_column_new();
	gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_GROW_ONLY);

	renderer = gtk_cell_renderer_pixbuf_new();
	gtk_tree_view_column_pack_start(column, renderer, FALSE);
	gtk_tree_view_column_add_attribute(column, renderer, "pixbuf", DIR_COLUMN_ICON);
	gtk_tree_view_column_set_cell_data_func(column, renderer, vdtree_color_cb, vd, NULL);

	renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_column_pack_start(column, renderer, TRUE);
	gtk_tree_view_column_add_attribute(column, renderer, "text", DIR_COLUMN_NAME);
	gtk_tree_view_column_set_cell_data_func(column, renderer, vdtree_color_cb, vd, NULL);

	gtk_tree_view_append_column(GTK_TREE_VIEW(vd->view), column);

	g_signal_connect(G_OBJECT(vd->view), "key_press_event",
			 G_CALLBACK(vdtree_press_key_cb), vd);

	gtk_container_add(GTK_CONTAINER(vd->widget), vd->view);
	gtk_widget_show(vd->view);

	vd->pf = folder_icons_new();

	vdtree_setup_root(vd);

	vdtree_dnd_init(vd);

	g_signal_connect(G_OBJECT(vd->view), "button_press_event",
			 G_CALLBACK(vdtree_press_cb), vd);
	g_signal_connect(G_OBJECT(vd->view), "button_release_event",
			 G_CALLBACK(vdtree_release_cb), vd);

	vdtree_set_path(vd, path);

	return vd;
}

#if 0
void vdtree_set_click_func(ViewDir *vd,
			   void (*func)(ViewDir *vd, GdkEventButton *event, FileData *fd, gpointer), gpointer data)
{
	if (!td) return;
	vd->click_func = func;
	vd->click_data = data;
}
#endif


