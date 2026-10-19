/*
 * netsurf-ng: more of the web platform, made the first time a page names
 * one of its exports (qjsky.c's modules):
 *
 * - DOMRect and DOMRectReadOnly (getBoundingClientRect's results);
 * - constructors for interfaces the bindings make only from C: Text,
 *   Comment, DocumentFragment, Image, Option, the UI events (MouseEvent,
 *   KeyboardEvent, UIEvent, FocusEvent, InputEvent, WheelEvent,
 *   PointerEvent): each keeps its interface's prototype, so instanceof
 *   and the bindings' methods work;
 * - MutationObserver over libdom's mutation events;
 * - IntersectionObserver and ResizeObserver from the layout's geometry
 *   (looked at again every half second while something is observed);
 * - FormData (from a form too), Blob, File, FileReader, DOMParser.
 *
 * The module: a function of the natives (iface: an interface object as the
 * bindings make it; mutation: a mutation event's details) whose result is
 * its exports.
 */
(function (natives) {
    "use strict";
    var g = globalThis, exports = {};
    var iface = natives.iface, mutation = natives.mutation;

    /* a constructor in place of an interface object: its prototype (and
     * the interface object's own prototype, for statics) kept
     */
    function constructible(name, f) {
	var orig = iface(name);
	if (orig) {
	    f.prototype = orig.prototype;
	    Object.setPrototypeOf(f, Object.getPrototypeOf(orig));
	    Object.getOwnPropertyNames(orig).forEach(function (k) {
		if (k !== "prototype" && k !== "length" && k !== "name" && !(k in f)) {
		    Object.defineProperty(f, k, Object.getOwnPropertyDescriptor(orig, k));
		}
	    });
	}
	Object.defineProperty(f.prototype, "constructor", { value: f, writable: true, configurable: true });
	exports[name] = f;
	return f;
    }
    function needNew(target, name) {
	if (!target) throw new TypeError(name + ": use new");
    }

    /* DOMRect */
    class DOMRectReadOnly {
	constructor(x, y, width, height) {
	    this._x = +(x || 0); this._y = +(y || 0);
	    this._w = +(width || 0); this._h = +(height || 0);
	}
	get x() { return this._x; }
	get y() { return this._y; }
	get width() { return this._w; }
	get height() { return this._h; }
	get top() { return Math.min(this._y, this._y + this._h); }
	get bottom() { return Math.max(this._y, this._y + this._h); }
	get left() { return Math.min(this._x, this._x + this._w); }
	get right() { return Math.max(this._x, this._x + this._w); }
	toJSON() {
	    return { x: this.x, y: this.y, width: this.width, height: this.height,
		     top: this.top, right: this.right, bottom: this.bottom, left: this.left };
	}
	static fromRect(r) { r = r || {}; return new this(r.x, r.y, r.width, r.height); }
    }
    class DOMRect extends DOMRectReadOnly {
	get x() { return this._x; } set x(v) { this._x = +v; }
	get y() { return this._y; } set y(v) { this._y = +v; }
	get width() { return this._w; } set width(v) { this._w = +v; }
	get height() { return this._h; } set height(v) { this._h = +v; }
    }
    exports.DOMRectReadOnly = DOMRectReadOnly;
    exports.DOMRect = DOMRect;

    /* nodes */
    constructible("Text", function Text(data) {
	needNew(new.target, "Text");
	return document.createTextNode(data === undefined ? "" : String(data));
    });
    constructible("Comment", function Comment(data) {
	needNew(new.target, "Comment");
	return document.createComment(data === undefined ? "" : String(data));
    });
    constructible("DocumentFragment", function DocumentFragment() {
	needNew(new.target, "DocumentFragment");
	return document.createDocumentFragment();
    });
    /* (the named constructors: their prototype is the element's) */
    var Image = function Image(width, height) {
	needNew(new.target, "Image");
	var img = document.createElement("img");
	if (width !== undefined) img.setAttribute("width", String(width >>> 0));
	if (height !== undefined) img.setAttribute("height", String(height >>> 0));
	return img;
    };
    Image.prototype = iface("HTMLImageElement") ? iface("HTMLImageElement").prototype : Image.prototype;
    exports.Image = Image;
    var Option = function Option(text, value, defaultSelected, selected) {
	needNew(new.target, "Option");
	var o = document.createElement("option");
	if (text !== undefined) o.textContent = String(text);
	if (value !== undefined) o.setAttribute("value", String(value));
	if (defaultSelected) o.setAttribute("selected", "");
	if (selected) o.selected = true;
	return o;
    };
    Option.prototype = iface("HTMLOptionElement") ? iface("HTMLOptionElement").prototype : Option.prototype;
    exports.Option = Option;

    /* the UI events: an Event made by the document, the interface's
     * prototype, the init dictionary's members as its own properties
     */
    var UI = { view: null, detail: 0, which: 0 };
    var MODIFIERS = { ctrlKey: false, shiftKey: false, altKey: false, metaKey: false };
    var MOUSE = Object.assign({ screenX: 0, screenY: 0, clientX: 0, clientY: 0, button: 0,
				buttons: 0, relatedTarget: null, movementX: 0, movementY: 0 }, MODIFIERS);
    var EVENTS = {
	UIEvent: UI,
	FocusEvent: Object.assign({ relatedTarget: null }, UI),
	MouseEvent: Object.assign({}, UI, MOUSE),
	WheelEvent: Object.assign({}, UI, MOUSE, { deltaX: 0, deltaY: 0, deltaZ: 0, deltaMode: 0 }),
	PointerEvent: Object.assign({}, UI, MOUSE, { pointerId: 0, width: 1, height: 1, pressure: 0,
						     tiltX: 0, tiltY: 0, pointerType: "", isPrimary: false }),
	KeyboardEvent: Object.assign({ key: "", code: "", location: 0, repeat: false,
				       isComposing: false, charCode: 0, keyCode: 0 }, UI, MODIFIERS),
	InputEvent: Object.assign({ data: null, inputType: "", isComposing: false }, UI)
    };
    var PARENT = { FocusEvent: "UIEvent", MouseEvent: "UIEvent", WheelEvent: "MouseEvent",
		   PointerEvent: "MouseEvent", KeyboardEvent: "UIEvent", InputEvent: "UIEvent",
		   UIEvent: "Event" };
    var protos = {};
    function eventProto(name) {
	if (protos[name]) return protos[name];
	var orig = iface(name);
	var p = orig ? orig.prototype : Object.create(eventProto(PARENT[name] || "Event") ||
						       g.Event.prototype);
	protos[name] = p;
	return p;
    }
    protos.Event = g.Event.prototype;
    Object.keys(EVENTS).forEach(function (name) {
	var defaults = EVENTS[name];
	var f = function (type, init) {
	    needNew(new.target, name);
	    if (arguments.length < 1) throw new TypeError(name + ": 1 argument required");
	    init = init || {};
	    var e = new g.Event(type, init);
	    Object.setPrototypeOf(e, f.prototype);
	    var values = {};
	    Object.keys(defaults).forEach(function (k) { values[k] = k in init ? init[k] : defaults[k]; });
	    if (name === "KeyboardEvent" && !("keyCode" in init) && typeof init.key === "string" &&
		init.key.length === 1) {
		values.keyCode = values.which = init.key.toUpperCase().charCodeAt(0);
	    }
	    Object.keys(values).forEach(function (k) {
		Object.defineProperty(e, k, { value: values[k], enumerable: true });
	    });
	    return e;
	};
	Object.defineProperty(f, "name", { value: name });
	var orig = iface(name);
	f.prototype = eventProto(name);
	if (!orig) {
	    /* not in the IDL: a prototype of its own under its parent's */
	    Object.defineProperty(f.prototype, Symbol.toStringTag, { value: name });
	}
	Object.setPrototypeOf(f, PARENT[name] && exports[PARENT[name]] ? exports[PARENT[name]] : g.Event);
	Object.defineProperty(f.prototype, "constructor", { value: f, writable: true, configurable: true });
	exports[name] = f;
    });

    /* MutationObserver: libdom's mutation events (DOMNodeInserted,
     * DOMNodeRemoved, DOMAttrModified, DOMCharacterDataModified) turned
     * into records, delivered in a microtask
     */
    var MUTATIONS = ["DOMNodeInserted", "DOMNodeRemoved", "DOMAttrModified", "DOMCharacterDataModified"];
    function MutationRecord(o) { Object.assign(this, o); }
    function MutationObserver(callback) {
	needNew(new.target, "MutationObserver");
	if (typeof callback !== "function") throw new TypeError("MutationObserver: not a function");
	this._cb = callback; this._records = []; this._targets = []; this._queued = false;
    }
    MutationObserver.prototype = {
	observe: function (target, options) {
	    options = Object.assign({}, options);
	    if (options.attributeOldValue || options.attributeFilter) {
		if (options.attributes === undefined) options.attributes = true;
	    }
	    if (options.characterDataOldValue && options.characterData === undefined) options.characterData = true;
	    if (!options.childList && !options.attributes && !options.characterData) {
		throw new TypeError("MutationObserver: childList, attributes or characterData needed");
	    }
	    var self = this, t = this._targets.find(function (t) { return t.node === target; });
	    if (t) {
		t.options = options;
		return;
	    }
	    t = { node: target, options: options, handler: function (e) { self._event(t, e); } };
	    this._targets.push(t);
	    MUTATIONS.forEach(function (type) { target.addEventListener(type, t.handler, true); });
	},
	disconnect: function () {
	    this._targets.forEach(function (t) {
		MUTATIONS.forEach(function (type) { t.node.removeEventListener(type, t.handler, true); });
	    });
	    this._targets = [];
	    this._records = [];
	},
	takeRecords: function () { var r = this._records; this._records = []; return r; },
	_event: function (t, e) {
	    var o = t.options, node = e.target, m = mutation(e) || {}, rec = null;
	    /* (libdom's attributes are Attr nodes: their insertion has no node here) */
	    if (!node || typeof node.nodeType !== "number") return;
	    switch (e.type) {
	    case "DOMNodeInserted":
	    case "DOMNodeRemoved":
		var parent = m.relatedNode || node.parentNode;
		/* (libdom's attributes are Attr nodes with text children: changes
		 * inside one are not the tree's) */
		if (!parent || parent.nodeType === 2 || node.nodeType === 2) return;
		if (!o.childList || (!o.subtree && parent !== t.node)) return;
		rec = { type: "childList", target: parent,
			addedNodes: e.type === "DOMNodeInserted" ? [node] : [],
			removedNodes: e.type === "DOMNodeRemoved" ? [node] : [],
			previousSibling: node.previousSibling, nextSibling: node.nextSibling,
			attributeName: null, attributeNamespace: null, oldValue: null };
		break;
	    case "DOMAttrModified":
		if (!o.attributes || (!o.subtree && node !== t.node)) return;
		if (o.attributeFilter && o.attributeFilter.indexOf(m.attrName) < 0) return;
		rec = { type: "attributes", target: node, addedNodes: [], removedNodes: [],
			previousSibling: null, nextSibling: null, attributeName: m.attrName || null,
			attributeNamespace: null,
			oldValue: o.attributeOldValue && m.attrChange !== 2 ? (m.prevValue === undefined ? null : m.prevValue) : null };
		break;
	    case "DOMCharacterDataModified":
		if (!o.characterData || (!o.subtree && node !== t.node)) return;
		rec = { type: "characterData", target: node, addedNodes: [], removedNodes: [],
			previousSibling: null, nextSibling: null, attributeName: null,
			attributeNamespace: null,
			oldValue: o.characterDataOldValue ? (m.prevValue === undefined ? null : m.prevValue) : null };
		break;
	    default:
		return;
	    }
	    this._records.push(new MutationRecord(rec));
	    if (!this._queued) {
		var self = this;
		this._queued = true;
		Promise.resolve().then(function () {
		    self._queued = false;
		    var r = self.takeRecords();
		    if (r.length) self._cb.call(self, r, self);
		});
	    }
	}
    };
    exports.MutationObserver = MutationObserver;
    exports.MutationRecord = MutationRecord;

    /* IntersectionObserver and ResizeObserver: the layout's geometry, looked
     * at when observing starts and every half second while anything is
     * observed (no scroll or layout events to go by)
     */
    var watchers = [], timer = null;
    function watch(w) {
	if (watchers.indexOf(w) < 0) watchers.push(w);
	if (timer === null) timer = setInterval(function () { watchers.slice().forEach(function (w) { w._check(false); }); }, 500);
    }
    function unwatch(w) {
	var i = watchers.indexOf(w);
	if (i >= 0) watchers.splice(i, 1);
	if (!watchers.length && timer !== null) { clearInterval(timer); timer = null; }
    }
    function margins(text) {
	var m = String(text || "0px").trim().split(/\s+/).map(function (v) { return parseFloat(v) || 0; });
	return [m[0], m[1] !== undefined ? m[1] : m[0], m[2] !== undefined ? m[2] : m[0],
		m[3] !== undefined ? m[3] : (m[1] !== undefined ? m[1] : m[0])];
    }
    function IntersectionObserver(callback, options) {
	needNew(new.target, "IntersectionObserver");
	if (typeof callback !== "function") throw new TypeError("IntersectionObserver: not a function");
	options = options || {};
	this._cb = callback;
	this.root = options.root || null;
	this.rootMargin = options.rootMargin || "0px 0px 0px 0px";
	var th = options.threshold === undefined ? [0] : [].concat(options.threshold);
	this.thresholds = th.map(Number).sort();
	this._targets = [];
	this._records = [];
    }
    IntersectionObserver.prototype = {
	observe: function (target) {
	    if (this._targets.some(function (t) { return t.node === target; })) return;
	    this._targets.push({ node: target, state: null });
	    watch(this);
	    var self = this;
	    setTimeout(function () { self._check(true); }, 0);
	},
	unobserve: function (target) {
	    this._targets = this._targets.filter(function (t) { return t.node !== target; });
	    if (!this._targets.length) unwatch(this);
	},
	disconnect: function () { this._targets = []; unwatch(this); },
	takeRecords: function () { var r = this._records; this._records = []; return r; },
	_check: function (first) {
	    var m = margins(this.rootMargin), root;
	    if (this.root) {
		root = this.root.getBoundingClientRect();
	    } else {
		root = new DOMRect(0, 0, g.innerWidth || 0, g.innerHeight || 0);
	    }
	    var top = root.top - m[0], right = root.right + m[1], bottom = root.bottom + m[2], left = root.left - m[3];
	    var self = this, now = g.performance ? performance.now() : Date.now();
	    this._targets.forEach(function (t) {
		var r = t.node.getBoundingClientRect();
		var ix = Math.max(0, Math.min(r.right, right) - Math.max(r.left, left));
		var iy = Math.max(0, Math.min(r.bottom, bottom) - Math.max(r.top, top));
		var area = r.width * r.height, inter = ix * iy;
		var shown = t.node.isConnected && (r.width > 0 || r.height > 0) &&
		    r.bottom >= top && r.top <= bottom && r.right >= left && r.left <= right;
		var ratio = !shown ? 0 : area > 0 ? inter / area : 1;
		var level = self.thresholds.filter(function (x) { return ratio >= x && (x > 0 || shown); }).length;
		var state = (shown ? 1 : 0) + ":" + level;
		if (state === t.state) return;
		t.state = state;
		self._records.push({ time: now, target: t.node, rootBounds: new DOMRect(left, top, right - left, bottom - top),
				     boundingClientRect: r, intersectionRect: new DOMRect(Math.max(r.left, left), Math.max(r.top, top), ix, iy),
				     isIntersecting: shown, intersectionRatio: ratio });
	    });
	    if (this._records.length) this._cb.call(this, this.takeRecords(), this);
	}
    };
    exports.IntersectionObserver = IntersectionObserver;
    function ResizeObserver(callback) {
	needNew(new.target, "ResizeObserver");
	if (typeof callback !== "function") throw new TypeError("ResizeObserver: not a function");
	this._cb = callback; this._targets = [];
    }
    ResizeObserver.prototype = {
	observe: function (target) {
	    if (this._targets.some(function (t) { return t.node === target; })) return;
	    this._targets.push({ node: target, size: null });
	    watch(this);
	    var self = this;
	    setTimeout(function () { self._check(true); }, 0);
	},
	unobserve: function (target) {
	    this._targets = this._targets.filter(function (t) { return t.node !== target; });
	    if (!this._targets.length) unwatch(this);
	},
	disconnect: function () { this._targets = []; unwatch(this); },
	_check: function () {
	    var entries = [];
	    this._targets.forEach(function (t) {
		var e = t.node, w = e.clientWidth || 0, h = e.clientHeight || 0, size = w + "x" + h;
		if (size === t.size) return;
		t.size = size;
		var box = [{ inlineSize: w, blockSize: h }];
		entries.push({ target: e, contentRect: new DOMRect(0, 0, w, h),
			       contentBoxSize: box, borderBoxSize: [{ inlineSize: e.offsetWidth || w, blockSize: e.offsetHeight || h }],
			       devicePixelContentBoxSize: box });
	    });
	    if (entries.length) this._cb.call(this, entries, this);
	}
    };
    exports.ResizeObserver = ResizeObserver;

    /* Blob, File, FileReader: bytes (Uint8Array) */
    function bytesOf(part) {
	if (part instanceof Blob) return part._b;
	if (part instanceof ArrayBuffer) return new Uint8Array(part.slice(0));
	if (ArrayBuffer.isView(part)) return new Uint8Array(part.buffer.slice(part.byteOffset, part.byteOffset + part.byteLength));
	return new TextEncoder().encode(String(part));
    }
    class Blob {
	constructor(parts, options) {
	    var chunks = (parts || []).map(bytesOf), n = 0;
	    chunks.forEach(function (c) { n += c.length; });
	    var b = new Uint8Array(n), at = 0;
	    chunks.forEach(function (c) { b.set(c, at); at += c.length; });
	    this._b = b;
	    this._type = options && options.type ? String(options.type).toLowerCase() : "";
	}
	get size() { return this._b.length; }
	get type() { return this._type; }
	slice(start, end, type) {
	    var b = new Blob([]);
	    b._b = this._b.slice(start === undefined ? 0 : start, end === undefined ? this._b.length : end);
	    b._type = type === undefined ? "" : String(type).toLowerCase();
	    return b;
	}
	text() { return Promise.resolve(new TextDecoder().decode(this._b)); }
	arrayBuffer() { return Promise.resolve(this._b.slice().buffer); }
	bytes() { return Promise.resolve(this._b.slice()); }
	/* for fetch and XMLHttpRequest (bodies are text), URL.createObjectURL */
	_nsBody() { return { text: new TextDecoder().decode(this._b), type: this._type || null }; }
	_nsDataURL() {
	    var s = "", b = this._b;
	    for (var i = 0; i < b.length; i += 8192) s += String.fromCharCode.apply(null, b.subarray(i, i + 8192));
	    return "data:" + (this._type || "application/octet-stream") + ";base64," + btoa(s);
	}
    }
    class File extends Blob {
	constructor(parts, name, options) {
	    super(parts, options);
	    this._name = String(name);
	    this._modified = options && options.lastModified !== undefined ? +options.lastModified : Date.now();
	}
	get name() { return this._name; }
	get lastModified() { return this._modified; }
    }
    exports.Blob = Blob;
    exports.File = File;
    function FileReader() {
	needNew(new.target, "FileReader");
	this.readyState = 0; this.result = null; this.error = null; this._l = {};
	this.onloadstart = this.onprogress = this.onload = this.onloadend = this.onerror = this.onabort = null;
    }
    FileReader.EMPTY = 0; FileReader.LOADING = 1; FileReader.DONE = 2;
    FileReader.prototype = {
	addEventListener: function (t, f) { (this._l[t] = this._l[t] || []).push(f); },
	removeEventListener: function (t, f) { this._l[t] = (this._l[t] || []).filter(function (x) { return x !== f; }); },
	_fire: function (t) {
	    var e = { type: t, target: this, currentTarget: this, loaded: this.result ? 1 : 0, total: 1 };
	    if (typeof this["on" + t] === "function") this["on" + t](e);
	    (this._l[t] || []).slice().forEach(function (f) { f.call(this, e); }, this);
	},
	_read: function (blob, how) {
	    if (this.readyState === 1) throw new DOMException("already reading", "InvalidStateError");
	    var self = this;
	    this.readyState = 1;
	    setTimeout(function () {
		self._fire("loadstart");
		self.result = how(blob);
		self.readyState = 2;
		self._fire("load");
		self._fire("loadend");
	    }, 0);
	},
	readAsText: function (blob) { this._read(blob, function (b) { return new TextDecoder().decode(b._b); }); },
	readAsArrayBuffer: function (blob) { this._read(blob, function (b) { return b._b.slice().buffer; }); },
	readAsDataURL: function (blob) { this._read(blob, function (b) { return b._nsDataURL(); }); },
	readAsBinaryString: function (blob) {
	    this._read(blob, function (b) { var s = ""; b._b.forEach(function (c) { s += String.fromCharCode(c); }); return s; });
	},
	abort: function () { this.readyState = 2; this.result = null; this._fire("abort"); this._fire("loadend"); }
    };
    exports.FileReader = FileReader;

    /* FormData */
    function FormData(form) {
	needNew(new.target, "FormData");
	this._l = [];
	if (form === undefined) return;
	var els = form.querySelectorAll("input, select, textarea, button");
	for (var i = 0; i < els.length; i++) {
	    var e = els[i], name = e.getAttribute("name"), tag = e.tagName.toUpperCase();
	    if (!name || e.hasAttribute("disabled")) continue;
	    if (tag === "INPUT") {
		var type = (e.getAttribute("type") || "text").toLowerCase();
		if (type === "submit" || type === "button" || type === "reset" || type === "image" || type === "file") continue;
		if ((type === "checkbox" || type === "radio") && !e.checked) continue;
		if (type === "checkbox" || type === "radio") {
		    this._l.push([name, e.hasAttribute("value") ? e.getAttribute("value") : "on"]);
		} else {
		    this._l.push([name, e.value === undefined ? (e.getAttribute("value") || "") : e.value]);
		}
	    } else if (tag === "SELECT") {
		var opts = e.querySelectorAll("option");
		for (var j = 0; j < opts.length; j++) {
		    if (opts[j].selected || opts[j].hasAttribute("selected")) {
			this._l.push([name, opts[j].getAttribute("value") !== null ? opts[j].getAttribute("value") : opts[j].textContent]);
		    }
		}
	    } else if (tag === "TEXTAREA") {
		this._l.push([name, e.value !== undefined ? e.value : e.textContent]);
	    }
	}
    }
    function entryValue(v, filename) {
	if (v instanceof Blob) {
	    if (!(v instanceof File) || filename !== undefined) {
		return new File([v], filename !== undefined ? String(filename) : (v instanceof File ? v.name : "blob"), { type: v.type });
	    }
	    return v;
	}
	return String(v);
    }
    FormData.prototype = {
	append: function (k, v, f) { this._l.push([String(k), entryValue(v, f)]); },
	delete: function (k) { k = String(k); this._l = this._l.filter(function (p) { return p[0] !== k; }); },
	get: function (k) { k = String(k); for (var p of this._l) if (p[0] === k) return p[1]; return null; },
	getAll: function (k) { k = String(k); return this._l.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); },
	has: function (k) { return this.get(k) !== null; },
	set: function (k, v, f) {
	    k = String(k); v = entryValue(v, f);
	    var i = this._l.findIndex(function (p) { return p[0] === k; });
	    if (i < 0) { this._l.push([k, v]); return; }
	    this._l[i][1] = v;
	    this._l = this._l.filter(function (p, j) { return j <= i || p[0] !== k; });
	},
	forEach: function (f, t) { this._l.forEach(function (p) { f.call(t, p[1], p[0], this); }, this); },
	entries: function () { return this._l.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); },
	keys: function () { return this._l.map(function (p) { return p[0]; })[Symbol.iterator](); },
	values: function () { return this._l.map(function (p) { return p[1]; })[Symbol.iterator](); },
	/* multipart/form-data (text: bodies are text) */
	_nsBody: function () {
	    var boundary = "----NetSurfFormBoundary" + Math.random().toString(36).slice(2, 14), out = "";
	    this._l.forEach(function (p) {
		var name = p[0].replace(/"/g, "%22").replace(/\r?\n/g, "%0A");
		out += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"";
		if (p[1] instanceof File) {
		    out += "; filename=\"" + p[1].name.replace(/"/g, "%22") + "\"\r\nContent-Type: " +
			(p[1].type || "application/octet-stream") + "\r\n\r\n" + new TextDecoder().decode(p[1]._b) + "\r\n";
		} else {
		    out += "\r\n\r\n" + p[1] + "\r\n";
		}
	    });
	    out += "--" + boundary + "--\r\n";
	    return { text: out, type: "multipart/form-data; boundary=" + boundary };
	}
    };
    FormData.prototype[Symbol.iterator] = FormData.prototype.entries;
    exports.FormData = FormData;

    /* DOMParser: HTML parsed into a new document (XML types too, as HTML) */
    constructible("DOMParser", function DOMParser() { needNew(new.target, "DOMParser"); });
    exports.DOMParser.prototype.parseFromString = function parseFromString(text, type) {
	var doc = document.implementation.createHTMLDocument("");
	doc.documentElement.innerHTML = String(text);
	return doc;
    };

    return exports;
})
