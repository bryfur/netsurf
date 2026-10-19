/*
 * Copyright 2026 Bryan Furia <bryan.furia@gmail.com>
 *
 * Event dispatch, handler attributes and the thread life cycle follow the
 * Duktape implementation (dukky.c), copyright 2012 Vincent Sanders,
 * 2015 Daniel Dilverstone, 2016 Michael Drake, 2016 John-Mark Bell.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * The JavaScript engine on QuickJS-ng.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <nsutils/time.h>

#include "netsurf/inttypes.h"
#include "utils/utils.h"
#include "utils/nsoption.h"
#include "utils/log.h"
#include "utils/corestrings.h"
#include "content/content.h"
#include "netsurf/browser_window.h"
#include "netsurf/console.h"
#include "netsurf/misc.h"
#include "content/fetch.h"
#include "content/llcache.h"
#include "desktop/gui_internal.h"
#include "utils/nsurl.h"
#include "html/private.h"

#include "javascript/js.h"
#include "javascript/content.h"

#include "quickjs/binding.h"
#include "quickjs/private.h"
#include "quickjs/prototype.h"
#include "quickjs/generics.js.inc"
#include "quickjs/polyfill.js.inc"

#include "qjsky.h"

#include <dom/dom.h>

#define JS_EXEC_TIMEOUT_MS 10000

/* the most C stack a page's scripts may use (the rest of NetSurf's stack is
 * for layout and the rest)
 */
#ifndef NETSURF_JS_STACK
#define NETSURF_JS_STACK (48 * 1024)
#endif

/* pointer -> object, one per page for nodes and for events (open
 * addressing, linear probing)
 */
struct qjsky_map {
	void **keys;
	JSValue *values;
	size_t size; /* a power of two, or 0 */
	size_t count;
};

/**
 * a heap: a runtime
 */
struct jsheap {
	JSRuntime *rt; /* made when a page first needs it */
	bool pending_destroy;
	unsigned int live_threads;
	uint64_t exec_start_time;
	JSAtom hidden[QJSKY_HIDDEN_COUNT];
};

/**
 * a thread: a page's browsing context, a context in the runtime
 */
struct jsthread {
	bool pending_destroy;
	bool failed; /* its context could not be made */
	unsigned int in_use;
	unsigned int running; /* script running (js_is_running) */
	jsheap *heap;
	JSContext *ctx; /* made when the page first needs it (thread_context) */
	void *win_priv; /* the browser window and content, for the Window */
	void *doc_priv;
	void *window; /* the Window's private struct (the global object's) */
	JSValue generics; /* generics.js's NetSurf object */
	struct qjsky_map nodes;
	struct qjsky_map events;
	struct js_fetch *fetches; /* the page's script fetches under way */
	JSValue rejections; /* promises rejected without a handler (an array) */
	int next_fetch_id;
	JSValue classes[2 * QJSKY_CLASS_COUNT];
};

static const char *const hidden_names[QJSKY_HIDDEN_COUNT] = {
	[QJSKY_HIDDEN_HANDLERS] = "handlers",
	[QJSKY_HIDDEN_LISTENERS] = "listeners",
	[QJSKY_HIDDEN_REGISTERED] = "registered",
	[QJSKY_HIDDEN_CLASS_LIST] = "classList",
	[QJSKY_HIDDEN_CHILD_NODES] = "childNodes",
	[QJSKY_HIDDEN_ATTRIBUTES] = "attributes",
	[QJSKY_HIDDEN_STYLE] = "style",
	[QJSKY_HIDDEN_NAVIGATOR] = "navigator",
	[QJSKY_HIDDEN_LOCATION] = "location",
	[QJSKY_HIDDEN_CONSOLE] = "console",
	[QJSKY_HIDDEN_CONSOLE_TIMERS] = "consoleTimers",
	[QJSKY_HIDDEN_IMPLEMENTATION] = "implementation",
	[QJSKY_HIDDEN_CONTEXT2D] = "context2d",
	[QJSKY_HIDDEN_DATA] = "data",
};

static inline jsthread *thread_of(JSContext *ctx)
{
	return JS_GetContextOpaque(ctx);
}

static inline jsheap *heap_of(JSContext *ctx)
{
	return JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
}

/*** pointer maps ***/

static size_t map_slot(const struct qjsky_map *map, const void *key)
{
	uintptr_t h = (uintptr_t)key;
	h ^= h >> 7;
	h *= 2654435761u;
	return (h ^ (h >> 15)) & (map->size - 1);
}

static bool map_grow(struct qjsky_map *map)
{
	struct qjsky_map bigger;
	size_t i;

	bigger.size = map->size == 0 ? 64 : map->size * 2;
	bigger.count = 0;
	bigger.keys = calloc(bigger.size, sizeof(void *));
	bigger.values = malloc(bigger.size * sizeof(JSValue));
	if (bigger.keys == NULL || bigger.values == NULL) {
		free(bigger.keys);
		free(bigger.values);
		return false;
	}
	for (i = 0; i < map->size; i++) {
		if (map->keys[i] != NULL) {
			size_t s = map_slot(&bigger, map->keys[i]);
			while (bigger.keys[s] != NULL) {
				s = (s + 1) & (bigger.size - 1);
			}
			bigger.keys[s] = map->keys[i];
			bigger.values[s] = map->values[i];
			bigger.count++;
		}
	}
	free(map->keys);
	free(map->values);
	*map = bigger;
	return true;
}

/** the value of key (not a new reference), JS_UNDEFINED if none */
static JSValue map_get(const struct qjsky_map *map, const void *key)
{
	size_t s;

	if (map->size == 0) {
		return JS_UNDEFINED;
	}
	for (s = map_slot(map, key); map->keys[s] != NULL;
	     s = (s + 1) & (map->size - 1)) {
		if (map->keys[s] == key) {
			return map->values[s];
		}
	}
	return JS_UNDEFINED;
}

/** key's value set (val taken; key not already there) */
static bool map_put(JSContext *ctx, struct qjsky_map *map, void *key, JSValue val)
{
	size_t s;

	if ((map->count + 1) * 4 > map->size * 3 && !map_grow(map)) {
		JS_FreeValue(ctx, val);
		return false;
	}
	s = map_slot(map, key);
	while (map->keys[s] != NULL) {
		s = (s + 1) & (map->size - 1);
	}
	map->keys[s] = key;
	map->values[s] = val;
	map->count++;
	return true;
}

/** key's entry removed (backward shift, no tombstones) */
static void map_remove(JSContext *ctx, struct qjsky_map *map, const void *key)
{
	size_t s, next;

	if (map->size == 0) {
		return;
	}
	for (s = map_slot(map, key); map->keys[s] != key;
	     s = (s + 1) & (map->size - 1)) {
		if (map->keys[s] == NULL) {
			return;
		}
	}
	JS_FreeValue(ctx, map->values[s]);
	map->keys[s] = NULL;
	map->count--;
	for (next = (s + 1) & (map->size - 1); map->keys[next] != NULL;
	     next = (next + 1) & (map->size - 1)) {
		size_t home = map_slot(map, map->keys[next]);
		/* does the entry at next belong at or before the hole? */
		if (((next - home) & (map->size - 1)) >=
		    ((next - s) & (map->size - 1))) {
			map->keys[s] = map->keys[next];
			map->values[s] = map->values[next];
			map->keys[next] = NULL;
			s = next;
		}
	}
}

static void map_free(JSContext *ctx, struct qjsky_map *map)
{
	size_t i;

	for (i = 0; i < map->size; i++) {
		if (map->keys[i] != NULL) {
			JS_FreeValue(ctx, map->values[i]);
		}
	}
	free(map->keys);
	free(map->values);
	memset(map, 0, sizeof(*map));
}

/*** what the generated bindings ask of us ***/

JSValue *qjsky_context_classes(JSContext *ctx)
{
	return thread_of(ctx)->classes;
}

void *qjsky_global_private(JSContext *ctx)
{
	void *priv = thread_of(ctx)->window;
	if (priv == NULL) {
		JS_ThrowTypeError(ctx, "Window is gone");
	}
	return priv;
}

/*** hidden values ***/

JSAtom qjsky_hidden(JSContext *ctx, enum qjsky_hidden which)
{
	jsheap *heap = heap_of(ctx);

	if (heap->hidden[which] == JS_ATOM_NULL) {
		JSValue sym = JS_NewPrivateSymbol(ctx, hidden_names[which]);
		if (JS_IsException(sym)) {
			return JS_ATOM_NULL;
		}
		heap->hidden[which] = JS_ValueToAtom(ctx, sym);
		JS_FreeValue(ctx, sym);
	}
	return heap->hidden[which];
}

JSValue qjsky_get_hidden(JSContext *ctx, JSValueConst obj, enum qjsky_hidden which)
{
	JSAtom atom = qjsky_hidden(ctx, which);
	if (atom == JS_ATOM_NULL || !JS_IsObject(obj)) {
		return JS_UNDEFINED;
	}
	return JS_GetProperty(ctx, obj, atom);
}

int qjsky_set_hidden(JSContext *ctx, JSValueConst obj, enum qjsky_hidden which, JSValue val)
{
	JSAtom atom = qjsky_hidden(ctx, which);
	if (atom == JS_ATOM_NULL) {
		JS_FreeValue(ctx, val);
		return -1;
	}
	return JS_DefinePropertyValue(ctx, obj, atom, val,
				      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0 ? -1 : 0;
}

JSValue qjsky_hidden_object(JSContext *ctx, JSValueConst obj, enum qjsky_hidden which)
{
	JSValue v = qjsky_get_hidden(ctx, obj, which);

	if (JS_IsUndefined(v)) {
		v = JS_NewObjectProto(ctx, JS_NULL);
		if (JS_IsException(v) ||
		    qjsky_set_hidden(ctx, obj, which, JS_DupValue(ctx, v)) < 0) {
			JS_FreeValue(ctx, v);
			return JS_EXCEPTION;
		}
	}
	return v;
}

/*** strings ***/

JSValue qjsky_string(JSContext *ctx, dom_string *str)
{
	if (str == NULL) {
		return JS_NewStringLen(ctx, "", 0);
	}
	return JS_NewStringLen(ctx, dom_string_data(str), dom_string_length(str));
}

dom_exception qjsky_dom_string(JSContext *ctx, JSValueConst val, dom_string **out)
{
	size_t len;
	const char *s = JS_ToCStringLen(ctx, &len, val);
	dom_exception exc;

	if (s == NULL) {
		return DOM_NO_MEM_ERR;
	}
	exc = dom_string_create((const uint8_t *)s, len, out);
	JS_FreeCString(ctx, s);
	if (exc != DOM_NO_ERR) {
		JS_ThrowOutOfMemory(ctx);
	}
	return exc;
}

JSValue qjsky_throw_dom(JSContext *ctx, dom_exception exc)
{
	static const char *const names[] = {
		[DOM_INDEX_SIZE_ERR] = "IndexSizeError",
		[DOM_DOMSTRING_SIZE_ERR] = "DOMStringSizeError",
		[DOM_HIERARCHY_REQUEST_ERR] = "HierarchyRequestError",
		[DOM_WRONG_DOCUMENT_ERR] = "WrongDocumentError",
		[DOM_INVALID_CHARACTER_ERR] = "InvalidCharacterError",
		[DOM_NO_DATA_ALLOWED_ERR] = "NoDataAllowedError",
		[DOM_NO_MODIFICATION_ALLOWED_ERR] = "NoModificationAllowedError",
		[DOM_NOT_FOUND_ERR] = "NotFoundError",
		[DOM_NOT_SUPPORTED_ERR] = "NotSupportedError",
		[DOM_INUSE_ATTRIBUTE_ERR] = "InUseAttributeError",
		[DOM_INVALID_STATE_ERR] = "InvalidStateError",
		[DOM_SYNTAX_ERR] = "SyntaxError",
		[DOM_INVALID_MODIFICATION_ERR] = "InvalidModificationError",
		[DOM_NAMESPACE_ERR] = "NamespaceError",
		[DOM_INVALID_ACCESS_ERR] = "InvalidAccessError",
		[DOM_VALIDATION_ERR] = "ValidationError",
		[DOM_TYPE_MISMATCH_ERR] = "TypeMismatchError",
	};
	const char *name = NULL;
	JSValue err;

	if (exc == DOM_NO_MEM_ERR) {
		return JS_ThrowOutOfMemory(ctx);
	}
	if ((unsigned)exc < sizeof(names) / sizeof(names[0])) {
		name = names[exc];
	}
	if (name == NULL) {
		return JS_ThrowInternalError(ctx, "DOM error %d", (int)exc);
	}
	err = JS_NewError(ctx);
	if (JS_IsException(err)) {
		return err;
	}
	JS_DefinePropertyValueStr(ctx, err, "name", JS_NewString(ctx, name),
				  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
	JS_DefinePropertyValueStr(ctx, err, "message", JS_NewString(ctx, name),
				  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
	JS_DefinePropertyValueStr(ctx, err, "code", JS_NewInt32(ctx, exc),
				  JS_PROP_CONFIGURABLE);
	return JS_Throw(ctx, err);
}

JSValue qjsky_string_result(JSContext *ctx, dom_exception exc, dom_string *str,
			    bool nullable)
{
	JSValue v;

	if (exc != DOM_NO_ERR) {
		return qjsky_throw_dom(ctx, exc);
	}
	if (str == NULL) {
		return nullable ? JS_NULL : JS_NewStringLen(ctx, "", 0);
	}
	v = qjsky_string(ctx, str);
	dom_string_unref(str);
	return v;
}

/*** nodes and events ***/

struct dom_node *qjsky_value_node(JSValueConst val)
{
	node_private_t *priv = qjsky_get_private(val, &qjsky_class_node);
	return priv != NULL ? priv->node : NULL;
}

JSValue qjsky_node_result(JSContext *ctx, dom_exception exc, struct dom_node *node)
{
	JSValue v;

	if (exc != DOM_NO_ERR) {
		return qjsky_throw_dom(ctx, exc);
	}
	v = qjsky_node(ctx, node);
	if (node != NULL) {
		dom_node_unref(node);
	}
	return v;
}

static const qjsky_class_t *html_element_class(dom_html_element_type type)
{
	switch (type) {
	case DOM_HTML_ELEMENT_TYPE_HTML: return &qjsky_class_html_html_element;
	case DOM_HTML_ELEMENT_TYPE_HEAD: return &qjsky_class_html_head_element;
	case DOM_HTML_ELEMENT_TYPE_META: return &qjsky_class_html_meta_element;
	case DOM_HTML_ELEMENT_TYPE_BASE: return &qjsky_class_html_base_element;
	case DOM_HTML_ELEMENT_TYPE_TITLE: return &qjsky_class_html_title_element;
	case DOM_HTML_ELEMENT_TYPE_BODY: return &qjsky_class_html_body_element;
	case DOM_HTML_ELEMENT_TYPE_DIV: return &qjsky_class_html_div_element;
	case DOM_HTML_ELEMENT_TYPE_FORM: return &qjsky_class_html_form_element;
	case DOM_HTML_ELEMENT_TYPE_LINK: return &qjsky_class_html_link_element;
	case DOM_HTML_ELEMENT_TYPE_BUTTON: return &qjsky_class_html_button_element;
	case DOM_HTML_ELEMENT_TYPE_INPUT: return &qjsky_class_html_input_element;
	case DOM_HTML_ELEMENT_TYPE_TEXTAREA: return &qjsky_class_html_text_area_element;
	case DOM_HTML_ELEMENT_TYPE_OPTGROUP: return &qjsky_class_html_opt_group_element;
	case DOM_HTML_ELEMENT_TYPE_OPTION: return &qjsky_class_html_option_element;
	case DOM_HTML_ELEMENT_TYPE_SELECT: return &qjsky_class_html_select_element;
	case DOM_HTML_ELEMENT_TYPE_HR: return &qjsky_class_html_hr_element;
	case DOM_HTML_ELEMENT_TYPE_DL: return &qjsky_class_html_dlist_element;
	case DOM_HTML_ELEMENT_TYPE_DIR: return &qjsky_class_html_directory_element;
	case DOM_HTML_ELEMENT_TYPE_MENU: return &qjsky_class_html_menu_element;
	case DOM_HTML_ELEMENT_TYPE_FIELDSET: return &qjsky_class_html_field_set_element;
	case DOM_HTML_ELEMENT_TYPE_LEGEND: return &qjsky_class_html_legend_element;
	case DOM_HTML_ELEMENT_TYPE_P: return &qjsky_class_html_paragraph_element;
	case DOM_HTML_ELEMENT_TYPE_H1:
	case DOM_HTML_ELEMENT_TYPE_H2:
	case DOM_HTML_ELEMENT_TYPE_H3:
	case DOM_HTML_ELEMENT_TYPE_H4:
	case DOM_HTML_ELEMENT_TYPE_H5:
	case DOM_HTML_ELEMENT_TYPE_H6: return &qjsky_class_html_heading_element;
	case DOM_HTML_ELEMENT_TYPE_BLOCKQUOTE:
	case DOM_HTML_ELEMENT_TYPE_Q: return &qjsky_class_html_quote_element;
	case DOM_HTML_ELEMENT_TYPE_PRE: return &qjsky_class_html_pre_element;
	case DOM_HTML_ELEMENT_TYPE_BR: return &qjsky_class_html_br_element;
	case DOM_HTML_ELEMENT_TYPE_LABEL: return &qjsky_class_html_label_element;
	case DOM_HTML_ELEMENT_TYPE_UL: return &qjsky_class_html_ulist_element;
	case DOM_HTML_ELEMENT_TYPE_OL: return &qjsky_class_html_olist_element;
	case DOM_HTML_ELEMENT_TYPE_LI: return &qjsky_class_html_li_element;
	case DOM_HTML_ELEMENT_TYPE_FONT: return &qjsky_class_html_font_element;
	case DOM_HTML_ELEMENT_TYPE_DEL:
	case DOM_HTML_ELEMENT_TYPE_INS: return &qjsky_class_html_mod_element;
	case DOM_HTML_ELEMENT_TYPE_A: return &qjsky_class_html_anchor_element;
	case DOM_HTML_ELEMENT_TYPE_IMG: return &qjsky_class_html_image_element;
	case DOM_HTML_ELEMENT_TYPE_OBJECT: return &qjsky_class_html_object_element;
	case DOM_HTML_ELEMENT_TYPE_PARAM: return &qjsky_class_html_param_element;
	case DOM_HTML_ELEMENT_TYPE_APPLET: return &qjsky_class_html_applet_element;
	case DOM_HTML_ELEMENT_TYPE_MAP: return &qjsky_class_html_map_element;
	case DOM_HTML_ELEMENT_TYPE_AREA: return &qjsky_class_html_area_element;
	case DOM_HTML_ELEMENT_TYPE_SCRIPT: return &qjsky_class_html_script_element;
	case DOM_HTML_ELEMENT_TYPE_CAPTION: return &qjsky_class_html_table_caption_element;
	case DOM_HTML_ELEMENT_TYPE_TD: return &qjsky_class_html_table_data_cell_element;
	case DOM_HTML_ELEMENT_TYPE_TH: return &qjsky_class_html_table_header_cell_element;
	case DOM_HTML_ELEMENT_TYPE_COL:
	case DOM_HTML_ELEMENT_TYPE_COLGROUP: return &qjsky_class_html_table_col_element;
	case DOM_HTML_ELEMENT_TYPE_THEAD:
	case DOM_HTML_ELEMENT_TYPE_TBODY:
	case DOM_HTML_ELEMENT_TYPE_TFOOT: return &qjsky_class_html_table_section_element;
	case DOM_HTML_ELEMENT_TYPE_TABLE: return &qjsky_class_html_table_element;
	case DOM_HTML_ELEMENT_TYPE_TR: return &qjsky_class_html_table_row_element;
	case DOM_HTML_ELEMENT_TYPE_STYLE: return &qjsky_class_html_style_element;
	case DOM_HTML_ELEMENT_TYPE_FRAMESET: return &qjsky_class_html_frame_set_element;
	case DOM_HTML_ELEMENT_TYPE_FRAME: return &qjsky_class_html_frame_element;
	case DOM_HTML_ELEMENT_TYPE_IFRAME: return &qjsky_class_html_iframe_element;
	case DOM_HTML_ELEMENT_TYPE_CANVAS: return &qjsky_class_html_canvas_element;
	case DOM_HTML_ELEMENT_TYPE_SPAN: return &qjsky_class_html_span_element;
	case DOM_HTML_ELEMENT_TYPE__UNKNOWN:
	case DOM_HTML_ELEMENT_TYPE__COUNT: return &qjsky_class_html_unknown_element;
	default:
		/* a known element without an interface of its own */
		return &qjsky_class_html_element;
	}
}

/** the interface of a node's object */
static const qjsky_class_t *node_class(struct dom_node *node)
{
	dom_node_type nodetype;
	dom_string *namespace;
	dom_html_element_type type;
	bool html;

	if (dom_node_get_node_type(node, &nodetype) != DOM_NO_ERR) {
		return &qjsky_class_node;
	}

	switch (nodetype) {
	case DOM_ELEMENT_NODE:
		if (dom_node_get_namespace(node, &namespace) != DOM_NO_ERR ||
		    namespace == NULL) {
			return &qjsky_class_element;
		}
		html = dom_string_isequal(namespace, corestring_dom_html_namespace);
		dom_string_unref(namespace);
		if (!html) {
			return &qjsky_class_element;
		}
		if (dom_html_element_get_tag_type(node, &type) != DOM_NO_ERR) {
			type = DOM_HTML_ELEMENT_TYPE__UNKNOWN;
		}
		return html_element_class(type);

	case DOM_TEXT_NODE:
		return &qjsky_class_text;

	case DOM_COMMENT_NODE:
		return &qjsky_class_comment;

	case DOM_DOCUMENT_NODE:
		return &qjsky_class_document;

	case DOM_ATTRIBUTE_NODE:
		return &qjsky_class_attr;

	default:
		return &qjsky_class_node;
	}
}

JSValue qjsky_node(JSContext *ctx, struct dom_node *node)
{
	jsthread *thread = thread_of(ctx);
	const qjsky_class_t *klass;
	JSValue obj;

	if (node == NULL) {
		return JS_NULL;
	}
	obj = map_get(&thread->nodes, node);
	if (!JS_IsUndefined(obj)) {
		return JS_DupValue(ctx, obj);
	}

	klass = node_class(node);
	obj = klass->create(ctx, node);
	if (JS_IsException(obj)) {
		return obj;
	}
	if (!map_put(ctx, &thread->nodes, node, JS_DupValue(ctx, obj))) {
		JS_FreeValue(ctx, obj);
		return JS_ThrowOutOfMemory(ctx);
	}
	return obj;
}

/* the interface of an event's object */
static const qjsky_class_t *event_class(dom_event *evt)
{
	const qjsky_class_t *klass = &qjsky_class_event;
	dom_string *type = NULL;

	if (dom_event_get_type(evt, &type) != DOM_NO_ERR) {
		return klass;
	}
	if (dom_string_isequal(type, corestring_dom_keydown) ||
	    dom_string_isequal(type, corestring_dom_keyup) ||
	    dom_string_isequal(type, corestring_dom_keypress)) {
		klass = &qjsky_class_keyboard_event;
	}
	dom_string_unref(type);
	return klass;
}

JSValue qjsky_event(JSContext *ctx, dom_event *evt)
{
	jsthread *thread = thread_of(ctx);
	JSValue obj = map_get(&thread->events, evt);

	if (!JS_IsUndefined(obj)) {
		return JS_DupValue(ctx, obj);
	}
	obj = event_class(evt)->create(ctx, evt);
	if (JS_IsException(obj)) {
		return obj;
	}
	if (!map_put(ctx, &thread->events, evt, JS_DupValue(ctx, obj))) {
		JS_FreeValue(ctx, obj);
		return JS_ThrowOutOfMemory(ctx);
	}
	return obj;
}

/*** running scripts ***/

static void reset_timeout(jsheap *heap)
{
	(void) nsu_getmonotonic_ms(&heap->exec_start_time);
}

void qjsky_reset_timeout(JSContext *ctx)
{
	reset_timeout(heap_of(ctx));
}

static int interrupt_handler(JSRuntime *rt, void *opaque)
{
	jsheap *heap = opaque;
	uint64_t now;

	if (heap->exec_start_time == 0) {
		return 0;
	}
	(void) nsu_getmonotonic_ms(&now);
	return now > heap->exec_start_time + JS_EXEC_TIMEOUT_MS;
}

void qjsky_log_exception(JSContext *ctx, const char *what)
{
	JSValue exc = JS_GetException(ctx);
	const char *msg = JS_ToCString(ctx, exc);
	JSValue stack = JS_UNDEFINED;
	const char *trace = NULL;

	if (JS_IsError(exc)) {
		stack = JS_GetPropertyStr(ctx, exc, "stack");
		if (!JS_IsUndefined(stack)) {
			trace = JS_ToCString(ctx, stack);
		}
	}
	NSLOG(jserrors, WARNING, "Uncaught error in JS (%s): %s%s%s", what,
	      msg != NULL ? msg : "?",
	      trace != NULL ? "\n" : "", trace != NULL ? trace : "");

	/* and on the page's console, as browsers do */
	{
		jsthread *thread = thread_of(ctx);
		window_private_t *win = thread != NULL ? thread->window : NULL;
		if (win != NULL && win->win != NULL && !win->closed_down) {
			char line[512];
			int len = snprintf(line, sizeof(line), "%s%s%s",
					   msg != NULL ? msg : "?",
					   trace != NULL ? "\n" : "",
					   trace != NULL ? trace : "");
			if (len > (int)sizeof(line) - 1) {
				len = sizeof(line) - 1;
			}
			browser_window_console_log(win->win, BW_CS_SCRIPT_ERROR,
						   line, len, BW_CS_FLAG_LEVEL_ERROR);
		}
	}
	JS_FreeCString(ctx, trace);
	JS_FreeCString(ctx, msg);
	JS_FreeValue(ctx, stack);
	JS_FreeValue(ctx, exc);
}

/* QuickJS's promise rejection tracker: a promise rejected with no handler
 * is kept until the job queue is empty (a handler may come meanwhile)
 */
static void rejection_tracker(JSContext *ctx, JSValueConst promise,
			      JSValueConst reason, bool is_handled, void *opaque)
{
	jsthread *thread = thread_of(ctx);
	int64_t len = 0, i;

	if (thread == NULL || !JS_IsArray(thread->rejections)) {
		return;
	}
	JS_GetLength(ctx, thread->rejections, &len);
	if (!is_handled) {
		JSValue pair = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, pair, 0, JS_DupValue(ctx, promise));
		JS_SetPropertyUint32(ctx, pair, 1, JS_DupValue(ctx, reason));
		JS_SetPropertyInt64(ctx, thread->rejections, len, pair);
		return;
	}
	for (i = 0; i < len; i++) {
		JSValue pair = JS_GetPropertyInt64(ctx, thread->rejections, i);
		JSValue p = JS_GetPropertyUint32(ctx, pair, 0);
		bool same = JS_IsStrictEqual(ctx, p, promise);
		JS_FreeValue(ctx, p);
		JS_FreeValue(ctx, pair);
		if (same) {
			JS_SetPropertyInt64(ctx, thread->rejections, i, JS_NULL);
			break;
		}
	}
}

/* the rejections still unhandled: logged as uncaught, as browsers do */
static void report_rejections(JSContext *ctx)
{
	jsthread *thread = thread_of(ctx);
	int64_t len = 0, i;

	if (thread == NULL || !JS_IsArray(thread->rejections) ||
	    JS_GetLength(ctx, thread->rejections, &len) < 0 || len == 0) {
		return;
	}
	for (i = 0; i < len; i++) {
		JSValue pair = JS_GetPropertyInt64(ctx, thread->rejections, i);
		if (JS_IsObject(pair)) {
			JS_Throw(ctx, JS_GetPropertyUint32(ctx, pair, 1));
			qjsky_log_exception(ctx, "in promise");
		}
		JS_FreeValue(ctx, pair);
	}
	JS_SetPropertyStr(ctx, thread->rejections, "length", JS_NewInt32(ctx, 0));
}

/* the jobs (promise reactions) queued run, then the rejections no handler
 * took reported
 */
static void run_jobs(JSContext *ctx)
{
	JSRuntime *rt = JS_GetRuntime(ctx);
	JSContext *job_ctx;
	int r;

	while ((r = JS_ExecutePendingJob(rt, &job_ctx)) != 0) {
		if (r < 0) {
			qjsky_log_exception(job_ctx, "job");
		}
	}
	report_rejections(ctx);
}

JSValue qjsky_call(JSContext *ctx, JSValueConst func, JSValueConst this_val,
		   int argc, JSValueConst *argv)
{
	jsthread *thread = thread_of(ctx);
	JSValue ret;

	thread->running++;
	reset_timeout(heap_of(ctx));
	ret = JS_Call(ctx, func, this_val, argc, argv);
	if (JS_IsException(ret)) {
		qjsky_log_exception(ctx, "callback");
	}
	run_jobs(ctx);
	thread->running--;
	return ret;
}

JSValue qjsky_eval(JSContext *ctx, const char *src, size_t len, const char *name)
{
	jsthread *thread = thread_of(ctx);
	JSValue ret;

	thread->running++;
	reset_timeout(heap_of(ctx));
	ret = JS_Eval(ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(ret)) {
		qjsky_log_exception(ctx, name);
	}
	run_jobs(ctx);
	thread->running--;
	return ret;
}

/* exported interface documented in js.h */
bool js_is_running(jsthread *thread)
{
	return thread != NULL && thread->running > 0;
}

JSValue qjsky_generic(JSContext *ctx, const char *name)
{
	return JS_GetPropertyStr(ctx, thread_of(ctx)->generics, name);
}

/*** event handlers and listeners ***/

/* the source of an element's on<name> attribute, JS_NULL if none */
static JSValue handler_attribute(JSContext *ctx, dom_string *name,
				 dom_event_target *et)
{
	dom_string *onname, *val;
	dom_node_type ntype;
	JSValue src;

	if (et == NULL ||
	    dom_node_get_node_type(et, &ntype) != DOM_NO_ERR ||
	    ntype != DOM_ELEMENT_NODE) {
		return JS_NULL;
	}
	if (dom_string_concat(corestring_dom_on, name, &onname) != DOM_NO_ERR) {
		return JS_NULL;
	}
	if (dom_element_get_attribute((dom_element *)et, onname, &val) != DOM_NO_ERR ||
	    val == NULL) {
		dom_string_unref(onname);
		return JS_NULL;
	}
	dom_string_unref(onname);
	src = qjsky_string(ctx, val);
	dom_string_unref(val);
	return src;
}

JSValue qjsky_get_event_handler(JSContext *ctx, JSValueConst target,
				dom_string *name, dom_event_target *et)
{
	JSValue handlers, handler, attr;
	JSAtom atom;
	const char *body;
	size_t body_len;
	char *src;
	size_t src_len;

	handlers = qjsky_get_hidden(ctx, target, QJSKY_HIDDEN_HANDLERS);
	if (JS_IsObject(handlers)) {
		atom = JS_NewAtomLen(ctx, dom_string_data(name), dom_string_length(name));
		handler = JS_GetProperty(ctx, handlers, atom);
		JS_FreeAtom(ctx, atom);
		JS_FreeValue(ctx, handlers);
		if (!JS_IsUndefined(handler)) {
			return handler;
		}
	} else {
		JS_FreeValue(ctx, handlers);
	}

	/* none set: the attribute's, compiled each time (it may change) */
	attr = handler_attribute(ctx, name, et);
	if (JS_IsNull(attr)) {
		return JS_NULL;
	}
	body = JS_ToCStringLen(ctx, &body_len, attr);
	JS_FreeValue(ctx, attr);
	if (body == NULL) {
		return JS_EXCEPTION;
	}
	src_len = body_len + 32;
	src = malloc(src_len);
	if (src == NULL) {
		JS_FreeCString(ctx, body);
		return JS_ThrowOutOfMemory(ctx);
	}
	src_len = snprintf(src, src_len, "(function (event) {\n%s\n})", body);
	JS_FreeCString(ctx, body);
	handler = JS_Eval(ctx, src, src_len, "on-attribute handler",
			  JS_EVAL_TYPE_GLOBAL);
	free(src);
	if (JS_IsException(handler)) {
		qjsky_log_exception(ctx, "compiling a handler attribute");
		return JS_NULL;
	}
	return handler;
}

JSValue qjsky_set_event_handler(JSContext *ctx, JSValueConst target,
				dom_string *name, JSValueConst handler,
				dom_event_target *et)
{
	JSValue handlers;
	JSAtom atom;
	int r;

	handlers = qjsky_hidden_object(ctx, target, QJSKY_HIDDEN_HANDLERS);
	if (JS_IsException(handlers)) {
		return handlers;
	}
	atom = JS_NewAtomLen(ctx, dom_string_data(name), dom_string_length(name));
	if (JS_IsFunction(ctx, handler)) {
		r = JS_SetProperty(ctx, handlers, atom, JS_DupValue(ctx, handler));
	} else {
		/* not a function: no handler (the attribute's either) */
		r = JS_SetProperty(ctx, handlers, atom, JS_NULL);
	}
	JS_FreeAtom(ctx, atom);
	JS_FreeValue(ctx, handlers);
	if (r < 0) {
		return JS_EXCEPTION;
	}
	qjsky_register_event_listener_for(ctx, (dom_element *)et, name, false);
	return JS_UNDEFINED;
}

/* the listeners of target for type, made when create (a new reference),
 * JS_UNDEFINED when there are none
 */
static JSValue listeners_for(JSContext *ctx, JSValueConst target,
			     JSValueConst type, bool create, bool *created)
{
	JSValue all, list;
	JSAtom atom;

	*created = false;
	all = create ? qjsky_hidden_object(ctx, target, QJSKY_HIDDEN_LISTENERS) :
		qjsky_get_hidden(ctx, target, QJSKY_HIDDEN_LISTENERS);
	if (!JS_IsObject(all)) {
		return all;
	}
	atom = JS_ValueToAtom(ctx, type);
	if (atom == JS_ATOM_NULL) {
		JS_FreeValue(ctx, all);
		return JS_EXCEPTION;
	}
	list = JS_GetProperty(ctx, all, atom);
	if (JS_IsUndefined(list) && create) {
		list = JS_NewArray(ctx);
		if (!JS_IsException(list) &&
		    JS_SetProperty(ctx, all, atom, JS_DupValue(ctx, list)) < 0) {
			JS_FreeValue(ctx, list);
			list = JS_EXCEPTION;
		}
		*created = !JS_IsException(list);
	}
	JS_FreeAtom(ctx, atom);
	JS_FreeValue(ctx, all);
	return list;
}

static int64_t listener_count(JSContext *ctx, JSValueConst list)
{
	int64_t len = 0;
	if (JS_GetLength(ctx, list, &len) < 0) {
		return -1;
	}
	return len;
}

/* the index of callback with flags in a listener list, -1 if not there */
static int64_t listener_index(JSContext *ctx, JSValueConst list,
			      JSValueConst callback, event_listener_flags flags)
{
	int64_t len = listener_count(ctx, list);
	int64_t i;

	for (i = 0; i < len; i++) {
		JSValue entry = JS_GetPropertyInt64(ctx, list, i);
		JSValue cb = JS_GetPropertyUint32(ctx, entry, 0);
		JSValue fl = JS_GetPropertyUint32(ctx, entry, 1);
		int32_t f = -1;
		bool same;

		JS_ToInt32(ctx, &f, fl);
		same = JS_IsStrictEqual(ctx, cb, callback) &&
			((f & ELF_CAPTURE) == (int32_t)(flags & ELF_CAPTURE));
		JS_FreeValue(ctx, fl);
		JS_FreeValue(ctx, cb);
		JS_FreeValue(ctx, entry);
		if (same) {
			return i;
		}
	}
	return -1;
}

int qjsky_listener_add(JSContext *ctx, JSValueConst target, JSValueConst type,
		       JSValueConst callback, event_listener_flags flags)
{
	bool created;
	JSValue list, entry;
	int64_t len;
	int r;

	list = listeners_for(ctx, target, type, true, &created);
	if (JS_IsException(list)) {
		return -1;
	}
	/* the same callback for the same phase is added once */
	if (listener_index(ctx, list, callback, flags) >= 0) {
		JS_FreeValue(ctx, list);
		return 0;
	}
	entry = JS_NewArray(ctx);
	if (JS_IsException(entry)) {
		JS_FreeValue(ctx, list);
		return -1;
	}
	JS_SetPropertyUint32(ctx, entry, 0, JS_DupValue(ctx, callback));
	JS_SetPropertyUint32(ctx, entry, 1, JS_NewInt32(ctx, flags));
	len = listener_count(ctx, list);
	r = JS_SetPropertyInt64(ctx, list, len, entry);
	JS_FreeValue(ctx, list);
	if (r < 0) {
		return -1;
	}
	return created ? 1 : 0;
}

/* the entry at idx out of a list (marked removed, for a dispatch under way
 * on a copy of the list)
 */
static void listener_drop(JSContext *ctx, JSValueConst list, int64_t idx)
{
	JSValue entry = JS_GetPropertyInt64(ctx, list, idx);
	JSValue splice, args[2], r;

	JS_SetPropertyUint32(ctx, entry, 2, JS_TRUE);
	JS_FreeValue(ctx, entry);

	splice = JS_GetPropertyStr(ctx, list, "splice");
	args[0] = JS_NewInt64(ctx, idx);
	args[1] = JS_NewInt32(ctx, 1);
	r = JS_Call(ctx, splice, list, 2, args);
	JS_FreeValue(ctx, r);
	JS_FreeValue(ctx, splice);
}

int qjsky_listener_remove(JSContext *ctx, JSValueConst target, JSValueConst type,
			  JSValueConst callback, event_listener_flags flags)
{
	bool created;
	JSValue list = listeners_for(ctx, target, type, false, &created);
	int64_t idx;

	if (JS_IsException(list)) {
		return -1;
	}
	if (JS_IsUndefined(list)) {
		return 0;
	}
	idx = listener_index(ctx, list, callback, flags);
	if (idx >= 0) {
		listener_drop(ctx, list, idx);
	}
	JS_FreeValue(ctx, list);
	return 0;
}

/* a listener's callback called: a function, or an object's handleEvent */
static void call_listener(JSContext *ctx, JSValueConst callback,
			  JSValueConst target, JSValueConst event)
{
	JSValue ret;

	if (JS_IsFunction(ctx, callback)) {
		ret = qjsky_call(ctx, callback, target, 1, &event);
	} else if (JS_IsObject(callback)) {
		JSValue handle = JS_GetPropertyStr(ctx, callback, "handleEvent");
		ret = JS_IsFunction(ctx, handle) ?
			qjsky_call(ctx, handle, callback, 1, &event) : JS_UNDEFINED;
		JS_FreeValue(ctx, handle);
	} else {
		ret = JS_UNDEFINED;
	}
	JS_FreeValue(ctx, ret);
}

/* target's listeners for type and this phase (capture or not) called
 * with event, on a copy of the list: listeners added meanwhile wait for
 * the next event, removed ones are marked (entry[2]) and skipped
 */
static void run_listeners(JSContext *ctx, JSValueConst target, JSValueConst type,
			  JSValueConst event, bool capture)
{
	JSValue list, copy;
	int64_t len, i;

	{
		bool created;
		list = listeners_for(ctx, target, type, false, &created);
	}
	if (!JS_IsObject(list)) {
		JS_FreeValue(ctx, list);
		return;
	}

	/* a copy: listeners added during the dispatch wait for the next one,
	 * removed ones are marked (entry[2]) and skipped
	 */
	copy = JS_NewArray(ctx);
	len = listener_count(ctx, list);
	for (i = 0; i < len; i++) {
		JS_SetPropertyInt64(ctx, copy, i, JS_GetPropertyInt64(ctx, list, i));
	}
	for (i = 0; i < len; i++) {
		JSValue entry = JS_GetPropertyInt64(ctx, copy, i);
		JSValue removed = JS_GetPropertyUint32(ctx, entry, 2);
		JSValue fl = JS_GetPropertyUint32(ctx, entry, 1);
		int32_t flags = 0;

		JS_ToInt32(ctx, &flags, fl);
		JS_FreeValue(ctx, fl);
		if (!JS_IsUndefined(removed) ||
		    capture != ((flags & ELF_CAPTURE) != 0)) {
			JS_FreeValue(ctx, removed);
			JS_FreeValue(ctx, entry);
			continue;
		}
		JS_FreeValue(ctx, removed);
		if (flags & ELF_ONCE) {
			JSValue cb = JS_GetPropertyUint32(ctx, entry, 0);
			int64_t idx = listener_index(ctx, list, cb, flags);
			if (idx >= 0) {
				listener_drop(ctx, list, idx);
			}
			JS_FreeValue(ctx, cb);
		}
		{
			JSValue cb = JS_GetPropertyUint32(ctx, entry, 0);
			call_listener(ctx, cb, target, event);
			JS_FreeValue(ctx, cb);
		}
		JS_FreeValue(ctx, entry);
	}
	JS_FreeValue(ctx, copy);
	JS_FreeValue(ctx, list);
}

/**
 * libdom's listener: the on<event> handler (target and bubbling phases)
 * and the listeners added for this phase.  A capture registration (pw's
 * low bit) runs the capturing listeners, a bubble registration the
 * others and the handler: at the target libdom calls both.
 */
static void generic_event_handler(dom_event *evt, void *pw)
{
	JSContext *ctx = (JSContext *)((uintptr_t)pw & ~(uintptr_t)1);
	bool capture = ((uintptr_t)pw & 1) != 0;
	jsthread *thread = thread_of(ctx);
	dom_string *name;
	dom_event_target *targ;
	JSValue node, event, type;

	if (thread == NULL || thread->pending_destroy) {
		return;
	}
	if (dom_event_get_type(evt, &name) != DOM_NO_ERR) {
		return;
	}
	if (dom_event_get_current_target(evt, &targ) != DOM_NO_ERR) {
		dom_string_unref(name);
		return;
	}

	thread->in_use++;
	node = qjsky_node(ctx, (struct dom_node *)targ);
	event = qjsky_event(ctx, evt);
	if (JS_IsException(node) || JS_IsException(event)) {
		qjsky_log_exception(ctx, "event");
		goto out;
	}

	if (!capture) {
		JSValue handler = qjsky_get_event_handler(ctx, node, name, targ);
		if (JS_IsFunction(ctx, handler)) {
			JSValue ret = qjsky_call(ctx, handler, node, 1,
						 (JSValueConst *)&event);
			if (JS_IsBool(ret) && !JS_ToBool(ctx, ret)) {
				dom_event_prevent_default(evt);
			}
			JS_FreeValue(ctx, ret);
		}
		JS_FreeValue(ctx, handler);
	}

	type = qjsky_string(ctx, name);
	run_listeners(ctx, node, type, event, capture);
	JS_FreeValue(ctx, type);

out:
	JS_FreeValue(ctx, event);
	JS_FreeValue(ctx, node);
	dom_node_unref(targ);
	dom_string_unref(name);
	thread->in_use--;
	if (thread->in_use == 0 && thread->pending_destroy) {
		js_destroythread(thread);
	}
}

void qjsky_register_event_listener_for(JSContext *ctx, struct dom_element *ele,
				       dom_string *name, bool capture)
{
	JSValue node, registered, flagv;
	JSAtom atom;
	int32_t flags = 0;
	int bit = capture ? 2 : 1;
	dom_event_listener *listen = NULL;

	/* the Window has no libdom listeners: NetSurf delivers its events */
	if (ele == NULL) {
		return;
	}
	node = qjsky_node(ctx, (struct dom_node *)ele);
	if (JS_IsException(node)) {
		qjsky_log_exception(ctx, "registering a listener");
		return;
	}
	registered = qjsky_hidden_object(ctx, node, QJSKY_HIDDEN_REGISTERED);
	JS_FreeValue(ctx, node);
	if (JS_IsException(registered)) {
		return;
	}
	atom = JS_NewAtomLen(ctx, dom_string_data(name), dom_string_length(name));
	flagv = JS_GetProperty(ctx, registered, atom);
	JS_ToInt32(ctx, &flags, flagv);
	JS_FreeValue(ctx, flagv);
	if (flags & bit) {
		JS_FreeAtom(ctx, atom);
		JS_FreeValue(ctx, registered);
		return;
	}
	JS_SetProperty(ctx, registered, atom, JS_NewInt32(ctx, flags | bit));
	JS_FreeAtom(ctx, atom);
	JS_FreeValue(ctx, registered);

	if (dom_event_listener_create(generic_event_handler,
				      (void *)((uintptr_t)ctx | (capture ? 1 : 0)),
				      &listen) != DOM_NO_ERR) {
		return;
	}
	if (dom_event_target_add_event_listener(ele, name, listen, capture) != DOM_NO_ERR) {
		NSLOG(netsurf, DEBUG, "Unable to register listener for %p.%*s",
		      ele, (int)dom_string_length(name), dom_string_data(name));
	}
	dom_event_listener_unref(listen);
}

/*** script fetches: the primitive of fetch() and XMLHttpRequest ***/

#define JS_FETCH_MAX_BODY (8 * 1024 * 1024)
#define JS_FETCH_MAX_REDIRECTS 20

struct js_fetch {
	struct js_fetch *next;
	jsthread *thread;
	int id;
	JSValue callback;
	struct fetch *fetch;
	nsurl *url;
	nsurl *referer;
	char *method;
	char **headers;	/* the script's, "Name: value", NULL terminated */
	char *body;	/* NUL terminated, or NULL */
	nsurl *origin;	/* the page's */
	bool cors;	/* cross origin: no cookies, the response checked */
	int redirects;
	char *rhead;	/* the response's header lines */
	size_t rhead_len;
	uint8_t *data;
	size_t len, cap;
};

static void js_fetch_start(void *p);

static unsigned int js_fetches; /* under way, in all threads */

/* exported interface documented in js.h */
unsigned int js_fetches_pending(void)
{
	return js_fetches;
}

static void js_fetch_free(struct js_fetch *jf)
{
	jsthread *thread = jf->thread;
	struct js_fetch **pp;
	int i;

	js_fetches--;
	for (pp = &thread->fetches; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == jf) {
			*pp = jf->next;
			break;
		}
	}
	guit->misc->schedule(-1, js_fetch_start, jf);
	if (jf->fetch != NULL) {
		fetch_abort(jf->fetch);
	}
	JS_FreeValue(thread->ctx, jf->callback);
	nsurl_unref(jf->url);
	if (jf->referer != NULL) {
		nsurl_unref(jf->referer);
	}
	if (jf->origin != NULL) {
		nsurl_unref(jf->origin);
	}
	for (i = 0; jf->headers != NULL && jf->headers[i] != NULL; i++) {
		free(jf->headers[i]);
	}
	free(jf->headers);
	free(jf->method);
	free(jf->body);
	free(jf->rhead);
	free(jf->data);
	free(jf);
}

/* the result to the script: ("done", status, url, headers, ArrayBuffer,
 * redirected)
 * or ("error", message); the fetch freed
 */
static void js_fetch_deliver(struct js_fetch *jf, const char *error, int status)
{
	jsthread *thread = jf->thread;
	JSContext *ctx = thread->ctx;
	JSValue args[6], ret;
	int argc;

	if (thread->pending_destroy) {
		js_fetch_free(jf);
		return;
	}
	if (error == NULL && jf->cors) {
		/* cross origin: readable if the server says so */
		const char *acao = NULL;
		char *line;
		for (line = jf->rhead; line != NULL && *line != 0; ) {
			if (strncasecmp(line, "Access-Control-Allow-Origin:", 28) == 0) {
				acao = line + 28;
				break;
			}
			line = strchr(line, '\n');
			line = line != NULL ? line + 1 : NULL;
		}
		if (acao != NULL) {
			char *o = NULL;
			size_t olen = 0;
			while (*acao == ' ') {
				acao++;
			}
			nsurl_get(jf->origin, NSURL_SCHEME | NSURL_HOST | NSURL_PORT, &o, &olen);
			if (!(acao[0] == '*' && (acao[1] == '\r' || acao[1] == '\n')) &&
			    (o == NULL || strncmp(acao, o, olen) != 0 ||
			     (acao[olen] != '\r' && acao[olen] != '\n'))) {
				acao = NULL;
			}
			free(o);
		}
		if (acao == NULL) {
			error = "a cross-origin response the server does not allow";
		}
	}

	thread->in_use++;
	if (error != NULL) {
		args[0] = JS_NewString(ctx, "error");
		args[1] = JS_NewString(ctx, error);
		argc = 2;
	} else {
		args[0] = JS_NewString(ctx, "done");
		args[1] = JS_NewInt32(ctx, status);
		args[2] = JS_NewString(ctx, nsurl_access(jf->url));
		args[3] = JS_NewString(ctx, jf->rhead != NULL ? jf->rhead : "");
		args[4] = JS_NewArrayBufferCopy(ctx, jf->data, jf->len);
		args[5] = JS_NewBool(ctx, jf->redirects > 0);
		argc = 6;
	}
	ret = qjsky_call(ctx, jf->callback, JS_UNDEFINED, argc, (JSValueConst *)args);
	JS_FreeValue(ctx, ret);
	while (argc-- > 0) {
		JS_FreeValue(ctx, args[argc]);
	}
	js_fetch_free(jf);
	thread->in_use--;
	if (thread->in_use == 0 && thread->pending_destroy) {
		js_destroythread(thread);
	}
}

static void js_fetch_callback(const fetch_msg *msg, void *p)
{
	struct js_fetch *jf = p;

	switch (msg->type) {
	case FETCH_HEADER: {
		/* "Name: value" lines, kept as they come */
		size_t n = msg->data.header_or_data.len;
		char *h = realloc(jf->rhead, jf->rhead_len + n + 2);
		if (h != NULL) {
			memcpy(h + jf->rhead_len, msg->data.header_or_data.buf, n);
			jf->rhead_len += n;
			if (jf->rhead_len == 0 || h[jf->rhead_len - 1] != '\n') {
				h[jf->rhead_len++] = '\n';
			}
			h[jf->rhead_len] = 0;
			jf->rhead = h;
		}
		break;
	}
	case FETCH_DATA: {
		size_t n = msg->data.header_or_data.len;
		if (jf->len + n > JS_FETCH_MAX_BODY) {
			fetch_abort(jf->fetch);
			jf->fetch = NULL;
			js_fetch_deliver(jf, "the response is too big", 0);
			return;
		}
		if (jf->len + n > jf->cap) {
			size_t cap = (jf->len + n) * 2;
			uint8_t *d = realloc(jf->data, cap);
			if (d == NULL) {
				break;
			}
			jf->data = d;
			jf->cap = cap;
		}
		memcpy(jf->data + jf->len, msg->data.header_or_data.buf, n);
		jf->len += n;
		break;
	}
	case FETCH_FINISHED:
	case FETCH_NOTMODIFIED: {
		int status = fetch_http_code(jf->fetch);
		jf->fetch = NULL; /* the fetch layer frees it */
		js_fetch_deliver(jf, NULL, status != 0 ? status : 200);
		break;
	}
	case FETCH_REDIRECT: {
		int status = fetch_http_code(jf->fetch);
		nsurl *to;
		jf->fetch = NULL;
		if (jf->redirects++ >= JS_FETCH_MAX_REDIRECTS ||
		    nsurl_join(jf->url, msg->data.redirect, &to) != NSERROR_OK) {
			js_fetch_deliver(jf, "too many redirects, or a bad one", 0);
			break;
		}
		nsurl_unref(jf->url);
		jf->url = to;
		/* 303 (and 301, 302 from a POST): a GET, no body */
		if (status == 303 || ((status == 301 || status == 302) &&
				      strcmp(jf->method, "POST") == 0)) {
			free(jf->method);
			jf->method = strdup("GET");
			free(jf->body);
			jf->body = NULL;
		}
		free(jf->rhead);
		jf->rhead = NULL;
		jf->rhead_len = 0;
		jf->len = 0;
		guit->misc->schedule(0, js_fetch_start, jf);
		break;
	}
	case FETCH_PROGRESS:
	case FETCH_CERTS:
		break;
	default:
		/* an error, a timeout, authentication, certificates */
		jf->fetch = NULL;
		js_fetch_deliver(jf, msg->type == FETCH_ERROR && msg->data.error != NULL ?
				 msg->data.error : "the fetch failed", 0);
		break;
	}
}

/* whether a cross-origin request is a simple one (no preflight needed) */
static bool js_fetch_simple(struct js_fetch *jf)
{
	int i;

	if (strcmp(jf->method, "GET") != 0 && strcmp(jf->method, "POST") != 0 &&
	    strcmp(jf->method, "HEAD") != 0) {
		return false;
	}
	for (i = 0; jf->headers != NULL && jf->headers[i] != NULL; i++) {
		const char *h = jf->headers[i];
		if (strncasecmp(h, "Accept:", 7) == 0 ||
		    strncasecmp(h, "Accept-Language:", 16) == 0 ||
		    strncasecmp(h, "Content-Language:", 17) == 0) {
			continue;
		}
		if (strncasecmp(h, "Content-Type:", 13) == 0) {
			const char *v = h + 13;
			while (*v == ' ') {
				v++;
			}
			if (strncasecmp(v, "text/plain", 10) == 0 ||
			    strncasecmp(v, "application/x-www-form-urlencoded", 33) == 0 ||
			    strncasecmp(v, "multipart/form-data", 19) == 0) {
				continue;
			}
		}
		return false;
	}
	return true;
}

/* (scheduled) the fetch started, or started again after a redirect */
static void js_fetch_start(void *p)
{
	struct js_fetch *jf = p;
	const char *hdrs[64];
	char method_hdr[48];
	char *origin_hdr = NULL;
	int n = 0, i;
	nserror err;

	jf->cors = nsurl_compare(jf->origin, jf->url,
				 NSURL_SCHEME | NSURL_HOST | NSURL_PORT) == false;
	if (jf->cors && !js_fetch_simple(jf)) {
		js_fetch_deliver(jf, "a cross-origin request that is not simple (preflight is not supported)", 0);
		return;
	}
	if (strcmp(jf->method, "GET") != 0 &&
	    !(strcmp(jf->method, "POST") == 0 && jf->body != NULL)) {
		/* HEAD: a GET whose body is dropped (fetch.js) */
		snprintf(method_hdr, sizeof(method_hdr), "X-NetSurf-Method: %s",
			 strcmp(jf->method, "HEAD") == 0 ? "GET" : jf->method);
		hdrs[n++] = method_hdr;
	}
	if (jf->cors) {
		char *o = NULL;
		size_t olen;
		hdrs[n++] = "X-NetSurf-No-Cookies: 1";
		if (nsurl_get(jf->origin, NSURL_SCHEME | NSURL_HOST | NSURL_PORT,
			      &o, &olen) == NSERROR_OK) {
			origin_hdr = malloc(olen + 9);
			if (origin_hdr != NULL) {
				sprintf(origin_hdr, "Origin: %s", o);
				hdrs[n++] = origin_hdr;
			}
			free(o);
		}
	}
	for (i = 0; jf->headers != NULL && jf->headers[i] != NULL && n < 62; i++) {
		hdrs[n++] = jf->headers[i];
	}
	hdrs[n] = NULL;

	err = fetch_start(jf->url, jf->referer, js_fetch_callback, jf, false,
			  (strcmp(jf->method, "GET") == 0 || strcmp(jf->method, "HEAD") == 0) ?
			  NULL : (jf->body != NULL ? jf->body : ""),
			  NULL, true, false, hdrs, &jf->fetch);
	free(origin_hdr);
	if (err != NSERROR_OK) {
		jf->fetch = NULL;
		js_fetch_deliver(jf, "the fetch could not start", 0);
	}
}

/* forbidden request headers (the fetch standard's): the browser's own */
static bool js_fetch_forbidden(const char *name)
{
	static const char *const forbidden[] = {
		"accept-charset", "accept-encoding", "access-control-request-headers",
		"access-control-request-method", "connection", "content-length",
		"cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive",
		"origin", "referer", "te", "trailer", "transfer-encoding", "upgrade",
		"user-agent", "via",
	};
	size_t i;

	if (strncasecmp(name, "proxy-", 6) == 0 || strncasecmp(name, "sec-", 4) == 0 ||
	    strncasecmp(name, "x-netsurf-", 10) == 0) {
		return true;
	}
	for (i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
		if (strcasecmp(name, forbidden[i]) == 0) {
			return true;
		}
	}
	return false;
}

/* __nsfetch(method, url, headers [[name, value], ...], body, callback):
 * an id (abortable with __nsfetch_abort(id))
 */
static JSValue js_nsfetch(JSContext *ctx, JSValueConst this_val, int argc,
			  JSValueConst *argv)
{
	jsthread *thread = thread_of(ctx);
	window_private_t *win = thread->window;
	struct js_fetch *jf;
	const char *s;
	nsurl *base;
	int64_t nh = 0, i;
	int k = 0;

	if (argc < 5 || !JS_IsFunction(ctx, argv[4]) || win == NULL ||
	    win->htmlc == NULL) {
		return JS_ThrowTypeError(ctx, "__nsfetch: bad arguments");
	}
	jf = calloc(1, sizeof(*jf));
	if (jf == NULL) {
		return JS_ThrowOutOfMemory(ctx);
	}
	jf->thread = thread;
	jf->callback = JS_DupValue(ctx, argv[4]);
	jf->id = ++thread->next_fetch_id;
	js_fetches++;

	s = JS_ToCString(ctx, argv[0]);
	jf->method = s != NULL ? strdup(s) : NULL;
	JS_FreeCString(ctx, s);
	if (jf->method != NULL) {
		char *m;
		for (m = jf->method; *m; m++) {
			*m = toupper((unsigned char)*m);
		}
	}

	base = win->htmlc->base_url != NULL ? win->htmlc->base_url :
		llcache_handle_get_url(win->htmlc->base.llcache);
	jf->origin = nsurl_ref(llcache_handle_get_url(win->htmlc->base.llcache));
	jf->referer = nsurl_ref(jf->origin);
	s = JS_ToCString(ctx, argv[1]);
	if (s == NULL || jf->method == NULL || nsurl_join(base, s, &jf->url) != NSERROR_OK) {
		JS_FreeCString(ctx, s);
		jf->url = nsurl_ref(base);
		jf->next = thread->fetches;
		thread->fetches = jf;
		js_fetch_free(jf);
		return JS_ThrowSyntaxError(ctx, "__nsfetch: not a URL");
	}
	JS_FreeCString(ctx, s);

	if (JS_IsArray(argv[2]) && JS_GetLength(ctx, argv[2], &nh) == 0 && nh > 0) {
		jf->headers = calloc(nh + 1, sizeof(char *));
		for (i = 0; jf->headers != NULL && i < nh && i < 60; i++) {
			JSValue pair = JS_GetPropertyInt64(ctx, argv[2], i);
			JSValue nv = JS_GetPropertyUint32(ctx, pair, 0);
			JSValue vv = JS_GetPropertyUint32(ctx, pair, 1);
			const char *name = JS_ToCString(ctx, nv);
			const char *value = JS_ToCString(ctx, vv);
			if (name != NULL && value != NULL && name[0] != 0 &&
			    strpbrk(name, ": \r\n") == NULL && strpbrk(value, "\r\n") == NULL &&
			    !js_fetch_forbidden(name)) {
				jf->headers[k] = malloc(strlen(name) + strlen(value) + 3);
				if (jf->headers[k] != NULL) {
					sprintf(jf->headers[k], "%s: %s", name, value);
					k++;
				}
			}
			JS_FreeCString(ctx, name);
			JS_FreeCString(ctx, value);
			JS_FreeValue(ctx, vv);
			JS_FreeValue(ctx, nv);
			JS_FreeValue(ctx, pair);
		}
	}
	if (!JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3])) {
		size_t blen;
		s = JS_ToCStringLen(ctx, &blen, argv[3]);
		if (s != NULL) {
			jf->body = malloc(blen + 1);
			if (jf->body != NULL) {
				memcpy(jf->body, s, blen + 1);
			}
		}
		JS_FreeCString(ctx, s);
	}

	jf->next = thread->fetches;
	thread->fetches = jf;
	guit->misc->schedule(0, js_fetch_start, jf);
	return JS_NewInt32(ctx, jf->id);
}

static JSValue js_nsfetch_abort(JSContext *ctx, JSValueConst this_val, int argc,
				JSValueConst *argv)
{
	jsthread *thread = thread_of(ctx);
	struct js_fetch *jf;
	int32_t id = 0;

	if (argc > 0) {
		JS_ToInt32(ctx, &id, argv[0]);
	}
	for (jf = thread->fetches; jf != NULL; jf = jf->next) {
		if (jf->id == id) {
			js_fetch_free(jf);
			break;
		}
	}
	return JS_UNDEFINED;
}

/* __nsutf8decode(buffer[, fatal]): a string of UTF-8 bytes;
 * __nsutf8encode(string): its UTF-8 bytes as a Uint8Array
 */
static JSValue js_utf8decode(JSContext *ctx, JSValueConst this_val, int argc,
			     JSValueConst *argv)
{
	size_t size, off = 0, len = 0, bpe;
	uint8_t *buf;
	JSValue abuf = JS_UNDEFINED;

	if (argc < 1) {
		return JS_NewStringLen(ctx, "", 0);
	}
	buf = JS_GetArrayBuffer(ctx, &size, argv[0]);
	if (buf != NULL) {
		len = size;
	} else {
		JS_FreeValue(ctx, JS_GetException(ctx));
		abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &bpe);
		if (JS_IsException(abuf)) {
			return JS_ThrowTypeError(ctx, "not a buffer");
		}
		buf = JS_GetArrayBuffer(ctx, &size, abuf);
		JS_FreeValue(ctx, abuf);
		if (buf == NULL) {
			return JS_ThrowTypeError(ctx, "a detached buffer");
		}
		buf += off;
	}
	return JS_NewStringLen(ctx, (const char *)buf, len);
}

static JSValue js_utf8encode(JSContext *ctx, JSValueConst this_val, int argc,
			     JSValueConst *argv)
{
	size_t len;
	const char *s = JS_ToCStringLen(ctx, &len, argc > 0 ? argv[0] : JS_UNDEFINED);
	JSValue abuf, args[3], arr;

	if (s == NULL) {
		return JS_EXCEPTION;
	}
	abuf = JS_NewArrayBufferCopy(ctx, (const uint8_t *)s, len);
	JS_FreeCString(ctx, s);
	if (JS_IsException(abuf)) {
		return abuf;
	}
	args[0] = abuf;
	args[1] = JS_NewInt32(ctx, 0);
	args[2] = JS_NewInt64(ctx, len);
	arr = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
	JS_FreeValue(ctx, abuf);
	return arr;
}

static const JSCFunctionListEntry js_natives[] = {
	JS_CFUNC_DEF("__nsfetch", 5, js_nsfetch),
	JS_CFUNC_DEF("__nsfetch_abort", 1, js_nsfetch_abort),
	JS_CFUNC_DEF("__nsutf8decode", 1, js_utf8decode),
	JS_CFUNC_DEF("__nsutf8encode", 1, js_utf8encode),
};

/*** the engine's interface (js.h) ***/

/* exported interface documented in js.h */
void js_initialise(void)
{
	javascript_init();
}

/* exported interface documented in js.h */
void js_finalise(void)
{
}

/* exported interface documented in js.h */
nserror js_newheap(int timeout, jsheap **heap)
{
	jsheap *ret = calloc(1, sizeof(*ret));

	*heap = NULL;
	if (ret == NULL) {
		return NSERROR_NOMEM;
	}
	*heap = ret;
	return NSERROR_OK;
}

/* the heap's runtime, made the first time a page needs one: NULL without
 * memory
 */
static JSRuntime *heap_runtime(jsheap *heap)
{
	if (heap->rt != NULL) {
		return heap->rt;
	}
	heap->rt = JS_NewRuntime();
	if (heap->rt == NULL) {
		return NULL;
	}
	JS_SetRuntimeOpaque(heap->rt, heap);
	JS_SetMaxStackSize(heap->rt, NETSURF_JS_STACK);
	JS_SetInterruptHandler(heap->rt, interrupt_handler, heap);
	JS_SetHostPromiseRejectionTracker(heap->rt, rejection_tracker, heap);
	if (qjsky_runtime_init(heap->rt) < 0) {
		JS_FreeRuntime(heap->rt);
		heap->rt = NULL;
	}
	return heap->rt;
}

static void destroyheap(jsheap *heap)
{
	int i;

	assert(heap->pending_destroy == true);
	assert(heap->live_threads == 0);
	if (heap->rt != NULL) {
		for (i = 0; i < QJSKY_HIDDEN_COUNT; i++) {
			if (heap->hidden[i] != JS_ATOM_NULL) {
				JS_FreeAtomRT(heap->rt, heap->hidden[i]);
			}
		}
		JS_FreeRuntime(heap->rt);
	}
	free(heap);
}

/* exported interface documented in js.h */
void js_destroyheap(jsheap *heap)
{
	heap->pending_destroy = true;
	if (heap->live_threads == 0) {
		destroyheap(heap);
	}
}

/* a script NetSurf brings with it run: 0, or -1 (logged) */
static int run_internal(JSContext *ctx, const unsigned char *src, size_t len,
			const char *name)
{
	char *buf = malloc(len + 1);
	JSValue r;

	if (buf == NULL) {
		return -1;
	}
	memcpy(buf, src, len);
	buf[len] = 0;
	r = JS_Eval(ctx, buf, len, name, JS_EVAL_TYPE_GLOBAL);
	free(buf);
	if (JS_IsException(r)) {
		qjsky_log_exception(ctx, name);
		return -1;
	}
	JS_FreeValue(ctx, r);
	return 0;
}

/* NETSURF_JS_MEM set: the runtime's memory at the stages of a page's
 * context, on stdout (what a page costs)
 */
static void mem_report(JSRuntime *rt, const char *stage)
{
	static int on = -1;
	JSMemoryUsage u;

	if (on < 0) {
		on = getenv("NETSURF_JS_MEM") != NULL;
	}
	if (!on) {
		return;
	}
	JS_ComputeMemoryUsage(rt, &u);
	printf("jsmem: %-12s %7lld bytes in %6lld blocks, %5lld objects, %5lld functions (%lld bytes of bytecode), %5lld atoms, %4lld shapes\n",
	       stage, (long long)u.malloc_size, (long long)u.malloc_count,
	       (long long)u.obj_count, (long long)u.js_func_count,
	       (long long)u.js_func_code_size, (long long)u.atom_count,
	       (long long)u.shape_count);
}

/* exported interface documented in js.h: the page recorded, its context
 * made when it first runs a script or has a handler (a page without
 * scripts costs nothing)
 */
nserror js_newthread(jsheap *heap, void *win_priv, void *doc_priv, jsthread **thread)
{
	jsthread *ret;
	int i;

	assert(heap != NULL);
	assert(heap->pending_destroy == false);

	ret = calloc(1, sizeof(*ret));
	if (ret == NULL) {
		return NSERROR_NOMEM;
	}
	ret->heap = heap;
	ret->win_priv = win_priv;
	ret->doc_priv = doc_priv;
	ret->generics = JS_UNDEFINED;
	ret->rejections = JS_UNDEFINED;
	for (i = 0; i < 2 * QJSKY_CLASS_COUNT; i++) {
		ret->classes[i] = JS_UNDEFINED;
	}
	heap->live_threads++;
	*thread = ret;
	return NSERROR_OK;
}

/* a page's context, made the first time (the Window, the polyfills): NULL
 * if it cannot be
 */
static JSContext *thread_context(jsthread *thread)
{
	JSRuntime *rt;
	JSContext *ctx;
	JSValue global, proto;

	if (thread->failed) {
		return NULL;
	}
	if (thread->ctx != NULL) {
		return thread->ctx;
	}
	rt = heap_runtime(thread->heap);
	if (rt == NULL) {
		thread->failed = true;
		return NULL;
	}

	mem_report(rt, "before");
	ctx = JS_NewContext(rt);
	if (ctx == NULL) {
		thread->failed = true;
		return NULL;
	}
	mem_report(rt, "context");
	thread->ctx = ctx;
	JS_SetContextOpaque(ctx, thread);
	thread->rejections = JS_NewArray(ctx);

	/* the global object is the Window */
	global = JS_GetGlobalObject(ctx);
	proto = qjsky_proto(ctx, &qjsky_class_window);
	if (JS_IsException(proto) ||
	    JS_SetPrototype(ctx, global, proto) < 0 ||
	    qjsky_define_interfaces(ctx, global) < 0) {
		JS_FreeValue(ctx, proto);
		JS_FreeValue(ctx, global);
		goto fail;
	}
	JS_FreeValue(ctx, proto);
	thread->window = qjsky_window___create_global(ctx, global,
						      thread->win_priv,
						      thread->doc_priv);
	if (thread->window == NULL) {
		JS_FreeValue(ctx, global);
		goto fail;
	}
	mem_report(rt, "window");

	/* the polyfills (which take the natives off the global), then
	 * NetSurf's generics (taken off the global)
	 */
	JS_SetPropertyFunctionList(ctx, global, js_natives,
				   sizeof(js_natives) / sizeof(js_natives[0]));
	if (run_internal(ctx, polyfill_js, polyfill_js_len, "polyfill.js") < 0 ||
	    run_internal(ctx, generics_js, generics_js_len, "generics.js") < 0) {
		JS_FreeValue(ctx, global);
		goto fail;
	}
	mem_report(rt, "polyfills");
	thread->generics = JS_GetPropertyStr(ctx, global, "NetSurf");
	{
		JSAtom atom = JS_NewAtom(ctx, "NetSurf");
		JS_DeleteProperty(ctx, global, atom, 0);
		JS_FreeAtom(ctx, atom);
	}
	JS_FreeValue(ctx, global);
	NSLOG(netsurf, DEBUG, "Thread %p's context made", thread);
	return ctx;

fail:
	NSLOG(netsurf, CRITICAL, "Unable to set up a JavaScript context");
	thread->failed = true;
	return NULL;
}

/* exported interface documented in js.h */
nserror js_closethread(jsthread *thread)
{
	if (thread->ctx != NULL && thread->window != NULL) {
		qjsky_window_closedown(thread->ctx, thread->window);
	}
	return NSERROR_OK;
}

static void destroythread(jsthread *thread)
{
	jsheap *heap = thread->heap;
	JSContext *ctx = thread->ctx;
	int i;

	assert(thread->in_use == 0);
	assert(thread->pending_destroy == true);

	if (ctx == NULL) {
		/* it never ran anything */
		free(thread);
		heap->live_threads--;
		if (heap->pending_destroy && heap->live_threads == 0) {
			destroyheap(heap);
		}
		return;
	}
	if (thread->window != NULL) {
		qjsky_window_closedown(ctx, thread->window);
	}
	while (thread->fetches != NULL) {
		js_fetch_free(thread->fetches);
	}
	map_free(ctx, &thread->events);
	map_free(ctx, &thread->nodes);
	JS_FreeValue(ctx, thread->generics);
	JS_FreeValue(ctx, thread->rejections);
	thread->rejections = JS_UNDEFINED;
	for (i = 0; i < 2 * QJSKY_CLASS_COUNT; i++) {
		JS_FreeValue(ctx, thread->classes[i]);
		thread->classes[i] = JS_UNDEFINED;
	}
	if (thread->window != NULL) {
		qjsky_window___destroy(heap->rt, thread->window);
		thread->window = NULL;
	}
	JS_SetContextOpaque(ctx, NULL);
	JS_FreeContext(ctx);
	free(thread);

	JS_RunGC(heap->rt);
	heap->live_threads--;
	if (heap->pending_destroy && heap->live_threads == 0) {
		destroyheap(heap);
	}
}

/* exported interface documented in js.h */
void js_destroythread(jsthread *thread)
{
	thread->pending_destroy = true;
	if (thread->in_use == 0) {
		destroythread(thread);
	}
}

/* exported interface documented in js.h */
bool js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen, const char *name)
{
	JSContext *ctx;
	JSValue r;
	char *buf;
	bool ret = false;

	if (txt == NULL || txtlen == 0 || thread->pending_destroy) {
		return false;
	}
	ctx = thread_context(thread);
	if (ctx == NULL) {
		return false;
	}

	/* QuickJS wants the source NUL terminated */
	buf = malloc(txtlen + 1);
	if (buf == NULL) {
		return false;
	}
	memcpy(buf, txt, txtlen);
	buf[txtlen] = 0;

	thread->in_use++;
	thread->running++;
	reset_timeout(thread->heap);
	r = JS_Eval(ctx, buf, txtlen, name != NULL ? name : "?unknown source?",
		    JS_EVAL_TYPE_GLOBAL);
	free(buf);
	if (JS_IsException(r)) {
		qjsky_log_exception(ctx, name != NULL ? name : "script");
	} else {
		ret = JS_IsBool(r) && JS_ToBool(ctx, r);
		JS_FreeValue(ctx, r);
	}
	run_jobs(ctx);
	thread->running--;
	thread->heap->exec_start_time = 0;

	thread->in_use--;
	if (thread->in_use == 0 && thread->pending_destroy) {
		destroythread(thread);
	}
	return ret;
}

/* exported interface documented in js.h */
void js_handle_new_element(jsthread *thread, struct dom_element *node)
{
	dom_namednodemap *map;
	dom_ulong idx, siz;
	dom_attr *attr = NULL;
	dom_string *key = NULL;
	dom_string *nodename;
	bool is_body;

	assert(thread);
	assert(node);

	if (dom_node_get_node_name(node, &nodename) != DOM_NO_ERR) {
		return;
	}
	is_body = (nodename == corestring_dom_BODY);
	dom_string_unref(nodename);

	if (dom_node_get_attributes(node, &map) != DOM_NO_ERR || map == NULL) {
		return;
	}

	thread->in_use++;
	if (dom_namednodemap_get_length(map, &siz) != DOM_NO_ERR) {
		goto out;
	}
	for (idx = 0; idx < siz; idx++) {
		if (dom_namednodemap_item(map, idx, &attr) != DOM_NO_ERR ||
		    dom_attr_get_name(attr, &key) != DOM_NO_ERR) {
			goto out;
		}
		/* the body's window events are the Window's (js_fire_event) */
		if (!(is_body && (key == corestring_dom_onblur ||
				  key == corestring_dom_onerror ||
				  key == corestring_dom_onfocus ||
				  key == corestring_dom_onload ||
				  key == corestring_dom_onresize ||
				  key == corestring_dom_onscroll)) &&
		    dom_string_length(key) > 2) {
			const uint8_t *data = (const uint8_t *)dom_string_data(key);
			if (data[0] == 'o' && data[1] == 'n') {
				dom_string *sub = NULL;
				if (dom_string_substr(key, 2, dom_string_length(key),
						      &sub) == DOM_NO_ERR) {
					JSContext *ctx = thread_context(thread);
					if (ctx != NULL) {
						qjsky_register_event_listener_for(
							ctx, node, sub, false);
					}
					dom_string_unref(sub);
				}
			}
		}
		dom_string_unref(key);
		key = NULL;
		dom_node_unref(attr);
		attr = NULL;
	}

out:
	if (key != NULL) {
		dom_string_unref(key);
	}
	if (attr != NULL) {
		dom_node_unref(attr);
	}
	dom_namednodemap_unref(map);
	thread->in_use--;
	if (thread->in_use == 0 && thread->pending_destroy) {
		destroythread(thread);
	}
}

/* exported interface documented in js.h */
void js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
	if (thread->ctx != NULL) {
		map_remove(thread->ctx, &thread->events, evt);
	}
}

/* a new event of a type (untrusted until dispatched by NetSurf), NULL
 * without memory
 */
static dom_event *new_event(dom_string *type, bool bubbles, bool cancelable)
{
	dom_event *evt;

	if (dom_event_create(&evt) != DOM_NO_ERR) {
		return NULL;
	}
	if (dom_event_init(evt, type, bubbles, cancelable) != DOM_NO_ERR) {
		dom_event_unref(evt);
		return NULL;
	}
	return evt;
}

/* the Window's listeners for an event (it is not a node: libdom does not
 * dispatch to it)
 */
static void window_listeners(JSContext *ctx, JSValueConst global, const char *type,
			     dom_event *evt)
{
	JSValue event = qjsky_event(ctx, evt);
	JSValue typev = JS_NewString(ctx, type);

	if (!JS_IsException(event)) {
		run_listeners(ctx, global, typev, event, false);
	}
	JS_FreeValue(ctx, typev);
	JS_FreeValue(ctx, event);
}

/* load at the Window: its handler (else the body's), its listeners */
static void fire_load(jsthread *thread, struct dom_document *doc)
{
	JSContext *ctx = thread->ctx;
	dom_event *evt = new_event(corestring_dom_load, false, false);
	dom_event_target *body;
	JSValue global, handler, event, ret;

	if (evt == NULL) {
		return;
	}
	global = JS_GetGlobalObject(ctx);
	handler = qjsky_get_event_handler(ctx, global, corestring_dom_load, NULL);
	if (!JS_IsFunction(ctx, handler) &&
	    dom_html_document_get_body(doc, &body) == DOM_NO_ERR && body != NULL) {
		JSValue bodynode = qjsky_node(ctx, (struct dom_node *)body);
		JS_FreeValue(ctx, handler);
		handler = JS_IsException(bodynode) ? JS_NULL :
			qjsky_get_event_handler(ctx, bodynode, corestring_dom_load, body);
		JS_FreeValue(ctx, bodynode);
		dom_node_unref(body);
	}
	if (JS_IsFunction(ctx, handler)) {
		event = qjsky_event(ctx, evt);
		if (!JS_IsException(event)) {
			ret = qjsky_call(ctx, handler, global, 1, (JSValueConst *)&event);
			JS_FreeValue(ctx, ret);
		}
		JS_FreeValue(ctx, event);
	}
	JS_FreeValue(ctx, handler);
	window_listeners(ctx, global, "load", evt);
	JS_FreeValue(ctx, global);
	js_event_cleanup(thread, evt);
	dom_event_unref(evt);
}

/* DOMContentLoaded: at the Document (bubbling), then the Window's */
static void fire_content_loaded(jsthread *thread, struct dom_document *doc)
{
	JSContext *ctx = thread->ctx;
	dom_string *type;
	dom_event *evt;
	JSValue global;
	bool success;

	if (dom_string_create((const uint8_t *)"DOMContentLoaded", 16, &type) != DOM_NO_ERR) {
		return;
	}
	evt = new_event(type, true, false);
	dom_string_unref(type);
	if (evt == NULL) {
		return;
	}
	dom_event_target_dispatch_event((dom_event_target *)doc, evt, &success);
	global = JS_GetGlobalObject(ctx);
	window_listeners(ctx, global, "DOMContentLoaded", evt);
	JS_FreeValue(ctx, global);
	js_event_cleanup(thread, evt);
	dom_event_unref(evt);
}

/* exported interface documented in js.h */
bool js_fire_event(jsthread *thread, const char *type, struct dom_document *doc,
		   struct dom_node *target)
{
	/** \todo events at other targets */
	if (target != NULL || thread->pending_destroy) {
		return true;
	}
	if (thread->ctx == NULL) {
		/* no scripts have run: no handlers or listeners, but for
		 * the body's onload attribute
		 */
		dom_html_element *body = NULL;
		bool has = false;
		if (strcmp(type, "load") != 0 ||
		    dom_html_document_get_body(doc, &body) != DOM_NO_ERR ||
		    body == NULL) {
			return true;
		}
		dom_element_has_attribute(body, corestring_dom_onload, &has);
		dom_node_unref(body);
		if (!has || thread_context(thread) == NULL) {
			return true;
		}
	}
	thread->in_use++;
	if (strcmp(type, "load") == 0) {
		fire_load(thread, doc);
	} else if (strcmp(type, "DOMContentLoaded") == 0) {
		fire_content_loaded(thread, doc);
	}
	thread->in_use--;
	if (thread->in_use == 0 && thread->pending_destroy) {
		destroythread(thread);
	}
	return true;
}
