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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <nsutils/time.h>

#include "netsurf/inttypes.h"
#include "utils/utils.h"
#include "utils/nsoption.h"
#include "utils/log.h"
#include "utils/corestrings.h"
#include "content/content.h"
#include "netsurf/browser_window.h"
#include "netsurf/console.h"

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
	JSRuntime *rt;
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
	unsigned int in_use;
	jsheap *heap;
	JSContext *ctx;
	void *window; /* the Window's private struct (the global object's) */
	JSValue generics; /* generics.js's NetSurf object */
	struct qjsky_map nodes;
	struct qjsky_map events;
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

/* the jobs (promise reactions) queued run */
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
}

JSValue qjsky_call(JSContext *ctx, JSValueConst func, JSValueConst this_val,
		   int argc, JSValueConst *argv)
{
	JSValue ret;

	reset_timeout(heap_of(ctx));
	ret = JS_Call(ctx, func, this_val, argc, argv);
	if (JS_IsException(ret)) {
		qjsky_log_exception(ctx, "callback");
	}
	run_jobs(ctx);
	return ret;
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
	JSValue node, event, type, list, copy;
	int64_t len, i;

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
	{
		bool created;
		list = listeners_for(ctx, node, type, false, &created);
	}
	JS_FreeValue(ctx, type);
	if (!JS_IsObject(list)) {
		JS_FreeValue(ctx, list);
		goto out;
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
			call_listener(ctx, cb, node, event);
			JS_FreeValue(ctx, cb);
		}
		JS_FreeValue(ctx, entry);
	}
	JS_FreeValue(ctx, copy);
	JS_FreeValue(ctx, list);

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
	ret->rt = JS_NewRuntime();
	if (ret->rt == NULL) {
		free(ret);
		return NSERROR_NOMEM;
	}
	JS_SetRuntimeOpaque(ret->rt, ret);
	JS_SetMaxStackSize(ret->rt, NETSURF_JS_STACK);
	JS_SetInterruptHandler(ret->rt, interrupt_handler, ret);
	if (qjsky_runtime_init(ret->rt) < 0) {
		JS_FreeRuntime(ret->rt);
		free(ret);
		return NSERROR_NOMEM;
	}

	*heap = ret;
	return NSERROR_OK;
}

static void destroyheap(jsheap *heap)
{
	int i;

	assert(heap->pending_destroy == true);
	assert(heap->live_threads == 0);
	for (i = 0; i < QJSKY_HIDDEN_COUNT; i++) {
		if (heap->hidden[i] != JS_ATOM_NULL) {
			JS_FreeAtomRT(heap->rt, heap->hidden[i]);
		}
	}
	JS_FreeRuntime(heap->rt);
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

/* exported interface documented in js.h */
nserror js_newthread(jsheap *heap, void *win_priv, void *doc_priv, jsthread **thread)
{
	jsthread *ret;
	JSValue global, proto;
	int i;

	assert(heap != NULL);
	assert(heap->pending_destroy == false);

	ret = calloc(1, sizeof(*ret));
	if (ret == NULL) {
		return NSERROR_NOMEM;
	}
	ret->heap = heap;
	ret->generics = JS_UNDEFINED;
	for (i = 0; i < 2 * QJSKY_CLASS_COUNT; i++) {
		ret->classes[i] = JS_UNDEFINED;
	}

	ret->ctx = JS_NewContext(heap->rt);
	if (ret->ctx == NULL) {
		free(ret);
		return NSERROR_NOMEM;
	}
	JS_SetContextOpaque(ret->ctx, ret);
	heap->live_threads++;

	/* the global object is the Window */
	global = JS_GetGlobalObject(ret->ctx);
	proto = qjsky_proto(ret->ctx, &qjsky_class_window);
	if (JS_IsException(proto) ||
	    JS_SetPrototype(ret->ctx, global, proto) < 0 ||
	    qjsky_define_interfaces(ret->ctx, global) < 0) {
		JS_FreeValue(ret->ctx, proto);
		JS_FreeValue(ret->ctx, global);
		goto fail;
	}
	JS_FreeValue(ret->ctx, proto);
	ret->window = qjsky_window___create_global(ret->ctx, global,
						   win_priv, doc_priv);
	if (ret->window == NULL) {
		JS_FreeValue(ret->ctx, global);
		goto fail;
	}

	/* the polyfills, then NetSurf's generics (taken off the global) */
	if (run_internal(ret->ctx, polyfill_js, polyfill_js_len, "polyfill.js") < 0 ||
	    run_internal(ret->ctx, generics_js, generics_js_len, "generics.js") < 0) {
		JS_FreeValue(ret->ctx, global);
		goto fail;
	}
	ret->generics = JS_GetPropertyStr(ret->ctx, global, "NetSurf");
	JS_DeleteProperty(ret->ctx, global, JS_NewAtom(ret->ctx, "NetSurf"), 0);
	JS_FreeValue(ret->ctx, global);

	NSLOG(netsurf, DEBUG, "New thread %p in heap %p", ret, heap);
	*thread = ret;
	return NSERROR_OK;

fail:
	NSLOG(netsurf, CRITICAL, "Unable to set up a JavaScript thread");
	ret->pending_destroy = true;
	js_destroythread(ret);
	return NSERROR_INIT_FAILED;
}

/* exported interface documented in js.h */
nserror js_closethread(jsthread *thread)
{
	if (thread->window != NULL) {
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

	if (thread->window != NULL) {
		qjsky_window_closedown(ctx, thread->window);
	}
	map_free(ctx, &thread->events);
	map_free(ctx, &thread->nodes);
	JS_FreeValue(ctx, thread->generics);
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
	ctx = thread->ctx;

	/* QuickJS wants the source NUL terminated */
	buf = malloc(txtlen + 1);
	if (buf == NULL) {
		return false;
	}
	memcpy(buf, txt, txtlen);
	buf[txtlen] = 0;

	thread->in_use++;
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
					qjsky_register_event_listener_for(
						thread->ctx, node, sub, false);
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
	map_remove(thread->ctx, &thread->events, evt);
}

/* exported interface documented in js.h */
bool js_fire_event(jsthread *thread, const char *type, struct dom_document *doc,
		   struct dom_node *target)
{
	JSContext *ctx = thread->ctx;
	dom_event *evt;
	dom_event_target *body;
	JSValue global, handler, event, ret;

	/* only load, at the Window: its handler, else the body's */
	if (target != NULL || strcmp(type, "load") != 0) {
		return true;
	}
	if (dom_event_create(&evt) != DOM_NO_ERR) {
		return true;
	}
	if (dom_event_init(evt, corestring_dom_load, false, false) != DOM_NO_ERR) {
		dom_event_unref(evt);
		return true;
	}

	thread->in_use++;
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
			ret = qjsky_call(ctx, handler, global, 1,
					 (JSValueConst *)&event);
			JS_FreeValue(ctx, ret);
		}
		JS_FreeValue(ctx, event);
	}
	JS_FreeValue(ctx, handler);
	JS_FreeValue(ctx, global);
	js_event_cleanup(thread, evt);
	dom_event_unref(evt);

	thread->in_use--;
	if (thread->in_use == 0 && thread->pending_destroy) {
		destroythread(thread);
	}
	return true;
}
