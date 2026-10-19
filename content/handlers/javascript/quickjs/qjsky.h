/*
 * Copyright 2026 Bryan Furia <bryan.furia@gmail.com>
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
 * The JavaScript engine on QuickJS-ng: what the bindings use.
 *
 * A heap is a JSRuntime, a thread (a page's browsing context) a JSContext
 * whose global object is the Window.  DOM objects are the generated
 * bindings' (binding.h): one JS class, the interface's private struct as
 * the opaque.
 */

#ifndef NETSURF_JAVASCRIPT_QJSKY_H
#define NETSURF_JAVASCRIPT_QJSKY_H

#include <stdbool.h>
#include <quickjs.h>
#include <dom/dom.h>

struct box;

/* addEventListener's options */
typedef enum {
	ELF_CAPTURE = 1 << 0,
	ELF_PASSIVE = 1 << 1,
	ELF_ONCE    = 1 << 2,
	ELF_NONE    = 0
} event_listener_flags;

/* Values the bindings keep on objects, under private symbols (invisible to
 * scripts)
 */
enum qjsky_hidden {
	QJSKY_HIDDEN_HANDLERS,          /**< on<event> handlers by event name */
	QJSKY_HIDDEN_LISTENERS,         /**< addEventListener's: name -> [[callback, flags, removed], ...] */
	QJSKY_HIDDEN_REGISTERED,        /**< event names libdom calls us for */
	QJSKY_HIDDEN_CLASS_LIST,
	QJSKY_HIDDEN_CHILD_NODES,
	QJSKY_HIDDEN_ATTRIBUTES,
	QJSKY_HIDDEN_STYLE,
	QJSKY_HIDDEN_NAVIGATOR,
	QJSKY_HIDDEN_LOCATION,
	QJSKY_HIDDEN_CONSOLE,
	QJSKY_HIDDEN_CONSOLE_TIMERS,
	QJSKY_HIDDEN_IMPLEMENTATION,
	QJSKY_HIDDEN_CONTEXT2D,
	QJSKY_HIDDEN_DATA,
	QJSKY_HIDDEN_SCREEN,
	QJSKY_HIDDEN_COUNT
};

/** The atom of a hidden value */
JSAtom qjsky_hidden(JSContext *ctx, enum qjsky_hidden which);

/** obj's hidden value (a new reference; JS_UNDEFINED if none) */
JSValue qjsky_get_hidden(JSContext *ctx, JSValueConst obj, enum qjsky_hidden which);

/** obj's hidden value set (val is taken): 0, or -1 with an exception */
int qjsky_set_hidden(JSContext *ctx, JSValueConst obj, enum qjsky_hidden which, JSValue val);

/** obj's hidden object, made empty when there is none (a new reference) */
JSValue qjsky_hidden_object(JSContext *ctx, JSValueConst obj, enum qjsky_hidden which);

/**
 * The object of a DOM node (memoised: the same node is the same object), a
 * new reference; JS_NULL for NULL, JS_EXCEPTION on failure
 */
JSValue qjsky_node(JSContext *ctx, struct dom_node *node);

/** The object of an event (memoised until js_event_cleanup()) */
JSValue qjsky_event(JSContext *ctx, dom_event *evt);

/** A JS string of a dom string ("" for NULL) */
JSValue qjsky_string(JSContext *ctx, dom_string *str);

/**
 * A dom string of a value (converted as String() does); DOM_NO_ERR, else
 * an exception is pending (DOM_NO_MEM_ERR)
 */
dom_exception qjsky_dom_string(JSContext *ctx, JSValueConst val, dom_string **out);

/**
 * A libdom error thrown as a DOMException would be (an Error whose name is
 * the exception's, e.g. "HierarchyRequestError", and code its number):
 * JS_EXCEPTION
 */
JSValue qjsky_throw_dom(JSContext *ctx, dom_exception exc);

/**
 * The result of a libdom call returning a node or a string: exc thrown if
 * not DOM_NO_ERR, else the node's object (JS_NULL for NULL) or the string
 * (JS_NULL for NULL when nullable, else ""); the node or string unref'd
 */
JSValue qjsky_node_result(JSContext *ctx, dom_exception exc, struct dom_node *node);
JSValue qjsky_string_result(JSContext *ctx, dom_exception exc, dom_string *str,
			    bool nullable);

/** The libdom node of a value, NULL if it is not a Node */
struct dom_node *qjsky_value_node(JSValueConst val);

/**
 * The current value of an on<event> handler of target (et its libdom
 * target, NULL for the Window): the function set, or one compiled from
 * the element's on<event> attribute, else JS_NULL
 */
JSValue qjsky_get_event_handler(JSContext *ctx, JSValueConst target,
				dom_string *name, dom_event_target *et);

/** An on<event> handler set (a function, or null to remove) */
JSValue qjsky_set_event_handler(JSContext *ctx, JSValueConst target,
				dom_string *name, JSValueConst handler,
				dom_event_target *et);

/**
 * libdom asked to call us for an event on an element (NULL: the Window,
 * whose events NetSurf delivers itself), once per name and phase
 */
void qjsky_register_event_listener_for(JSContext *ctx, struct dom_element *ele,
				       dom_string *name, bool capture);

/**
 * addEventListener/removeEventListener on target: a callback with flags
 * added (1 when the first for the type, 0 when not, -1 exception) or
 * removed (0, -1 exception)
 */
int qjsky_listener_add(JSContext *ctx, JSValueConst target, JSValueConst type,
		       JSValueConst callback, event_listener_flags flags);
int qjsky_listener_remove(JSContext *ctx, JSValueConst target, JSValueConst type,
			  JSValueConst callback, event_listener_flags flags);

/** A function of NetSurf's generics.js (a new reference) */
JSValue qjsky_generic(JSContext *ctx, const char *name);

/**
 * A function called as an event handler or timer would be: the exception
 * (if one is thrown) logged, the job queue run after; the result, or
 * JS_EXCEPTION
 */
JSValue qjsky_call(JSContext *ctx, JSValueConst func, JSValueConst this_val,
		   int argc, JSValueConst *argv);

/**
 * Source run as a global script as a timer's would be (the exception
 * logged, the job queue run after): the result, or JS_EXCEPTION.  src is
 * NUL terminated at src[len].
 */
JSValue qjsky_eval(JSContext *ctx, const char *src, size_t len, const char *name);

/** The pending exception logged (and cleared) */
void qjsky_log_exception(JSContext *ctx, const char *what);

/** The page's script timeout restarted (scripts run at most 10 s) */
void qjsky_reset_timeout(JSContext *ctx);

/**
 * The window's viewport (its size in pixels and how far it is scrolled; 0
 * when the page has no window) and the window scrolled to (x, y)
 */
void qjsky_viewport(JSContext *ctx, int *width, int *height, int *sx, int *sy);
void qjsky_scroll_to(JSContext *ctx, int x, int y);

/**
 * An element's border box in document coordinates from the layout: false
 * when it has none (not rendered, or the page not laid out yet)
 */
bool qjsky_node_box(struct dom_node *node, int *x, int *y, int *width,
		    int *height, struct box **box);

/** A DOMRect (web.js's) of x, y, width, height: a new reference */
/* A program's natives for a page (netsurf-ng as an app runtime, Photon):
 * called with each page's new context (the Window, the polyfills made) and
 * its URL; < 0 a failure (logged).  NULL: none.
 */
typedef int (*qjsky_native_hook_t)(JSContext *ctx, const char *url);
extern qjsky_native_hook_t qjsky_native_hook;

JSValue qjsky_style_declaration(JSContext *ctx, JSValueConst element);
JSValue qjsky_dom_rect(JSContext *ctx, double x, double y, double width,
		       double height);

/** The Window's private struct, its browsing context's closing down */
void qjsky_window_closedown(JSContext *ctx, void *window);

#endif
