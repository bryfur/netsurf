/*
 * netsurf-ng: the network from scripts, made the first time a page names
 * one of its exports (qjsky.c's modules).  XMLHttpRequest and fetch() over
 * the engine's primitive (natives.fetch: NetSurf's fetchers, the
 * same-origin and simple-CORS rules), Headers, Request, Response,
 * AbortController, AbortSignal, TextEncoder and TextDecoder (UTF-8),
 * URLSearchParams.
 *
 * The module: a function of the natives whose result is its exports.
 */
(function (natives) {
    "use strict";
    var nsfetch = natives.fetch, nsabort = natives.abort, urlparse = natives.urlparse;
    var utf8decode = natives.utf8decode, utf8encode = natives.utf8encode;
    var exports = {};

    function define(name, value) {
	exports[name] = value;
    }
    function domError(name, message) {
	return new DOMException(message || name, name);
    }

    var STATUS = {
	200: "OK", 201: "Created", 202: "Accepted", 204: "No Content", 206: "Partial Content",
	301: "Moved Permanently", 302: "Found", 303: "See Other", 304: "Not Modified",
	307: "Temporary Redirect", 308: "Permanent Redirect", 400: "Bad Request",
	401: "Unauthorized", 403: "Forbidden", 404: "Not Found", 405: "Method Not Allowed",
	409: "Conflict", 410: "Gone", 413: "Payload Too Large", 415: "Unsupported Media Type",
	429: "Too Many Requests", 500: "Internal Server Error", 501: "Not Implemented",
	502: "Bad Gateway", 503: "Service Unavailable", 504: "Gateway Timeout"
    };

    /* "Name: value" lines into [[lowercased name, value], ...] */
    function parseHeaders(text) {
	var out = [];
	text.split(/\r?\n/).forEach(function (line) {
	    var i = line.indexOf(":");
	    if (i > 0) out.push([line.slice(0, i).trim().toLowerCase(), line.slice(i + 1).trim()]);
	});
	return out;
    }

    /* TextEncoder, TextDecoder */
    function TextEncoder() {}
    TextEncoder.prototype = {
	encoding: "utf-8",
	encode: function (s) { return utf8encode(s === undefined ? "" : String(s)); }
    };
    function TextDecoder(label) {
	label = label === undefined ? "utf-8" : String(label).toLowerCase().trim();
	if (label !== "utf-8" && label !== "utf8" && label !== "unicode-1-1-utf-8") {
	    throw new RangeError("TextDecoder: only UTF-8, not " + label);
	}
	this.encoding = "utf-8";
    }
    TextDecoder.prototype.decode = function (buf) {
	return buf === undefined ? "" : utf8decode(buf);
    };
    define("TextEncoder", TextEncoder);
    define("TextDecoder", TextDecoder);

    /* URLSearchParams */
    function encodeForm(s) {
	return encodeURIComponent(s).replace(/%20/g, "+").replace(/[!'()~]/g, function (c) {
	    return "%" + c.charCodeAt(0).toString(16).toUpperCase();
	});
    }
    function decodeForm(s) {
	try { return decodeURIComponent(s.replace(/\+/g, " ")); } catch (e) { return s; }
    }
    function URLSearchParams(init) {
	var list = this._l = [];
	if (init === undefined || init === null) return;
	if (typeof init === "object") {
	    if (init instanceof URLSearchParams) init = init._l;
	    if (typeof init[Symbol.iterator] === "function") {
		for (var pair of init) list.push([String(pair[0]), String(pair[1])]);
	    } else {
		Object.keys(init).forEach(function (k) { list.push([k, String(init[k])]); });
	    }
	    return;
	}
	String(init).replace(/^\?/, "").split("&").forEach(function (part) {
	    if (!part) return;
	    var i = part.indexOf("=");
	    list.push(i < 0 ? [decodeForm(part), ""] : [decodeForm(part.slice(0, i)), decodeForm(part.slice(i + 1))]);
	});
    }
    URLSearchParams.prototype = {
	append: function (k, v) { this._l.push([String(k), String(v)]); changed(this); },
	delete: function (k) { k = String(k); this._l = this._l.filter(function (p) { return p[0] !== k; }); changed(this); },
	get: function (k) { k = String(k); for (var p of this._l) if (p[0] === k) return p[1]; return null; },
	getAll: function (k) { k = String(k); return this._l.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); },
	has: function (k) { return this.get(k) !== null; },
	set: function (k, v) {
	    k = String(k); v = String(v);
	    var i = this._l.findIndex(function (p) { return p[0] === k; });
	    if (i < 0) { this._l.push([k, v]); changed(this); return; }
	    this._l[i][1] = v;
	    this._l = this._l.filter(function (p, j) { return j <= i || p[0] !== k; });
	    changed(this);
	},
	sort: function () { this._l.sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }); changed(this); },
	toString: function () { return this._l.map(function (p) { return encodeForm(p[0]) + "=" + encodeForm(p[1]); }).join("&"); },
	forEach: function (f, t) { this._l.forEach(function (p) { f.call(t, p[1], p[0], this); }, this); },
	entries: function () { return this._l.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); },
	keys: function () { return this._l.map(function (p) { return p[0]; })[Symbol.iterator](); },
	values: function () { return this._l.map(function (p) { return p[1]; })[Symbol.iterator](); },
	get size() { return this._l.length; }
    };
    URLSearchParams.prototype[Symbol.iterator] = URLSearchParams.prototype.entries;
    /* a URL's searchParams tells it of changes */
    function changed(sp) {
	if (sp._url) sp._url._setSearch(sp.toString());
    }
    define("URLSearchParams", URLSearchParams);

    /* URL: parsed by NetSurf's nsurl (natives.urlparse: the parts, or null
     * when it is not a URL); a part set makes the URL again from the parts
     */
    var DEFAULT_PORTS = { "http:": "80", "https:": "443", "ws:": "80", "wss:": "443", "ftp:": "21" };
    function parseURL(input, base) {
	input = String(input).replace(/^[\u0000-\u0020]+|[\u0000-\u0020]+$/g, "").replace(/[\t\n\r]/g, "");
	if (base === null && !/^[A-Za-z][A-Za-z0-9+.-]*:/.test(input)) return null;
	var p = urlparse(input, base);
	if (!p) return null;
	p.protocol = p.protocol.replace(/:?$/, ":").toLowerCase();
	p.port = p.port.replace(/^:/, "");
	if (DEFAULT_PORTS[p.protocol] === p.port) p.port = "";
	p.search = p.search === "" || p.search === "?" ? "" : "?" + p.search.replace(/^\?/, "");
	p.hash = p.hash === "" || p.hash === "#" ? "" : "#" + p.hash.replace(/^#/, "");
	return p;
    }
    function compose(p) {
	var auth = "";
	if (p.hostname || p.protocol === "file:") {
	    auth = "//";
	    if (p.username) auth += p.username + (p.password ? ":" + p.password : "") + "@";
	    auth += p.hostname + (p.port ? ":" + p.port : "");
	}
	return p.protocol + auth + p.pathname + p.search + p.hash;
    }
    function URL(url, base) {
	if (!new.target) throw new TypeError("URL: use new");
	var b = null;
	if (base !== undefined) {
	    var bp = parseURL(base, null);
	    if (!bp) throw new TypeError("'" + base + "' is not a valid base URL");
	    b = bp.href;
	}
	var p = parseURL(url, b);
	if (!p) throw new TypeError("'" + url + "' is not a valid URL");
	this._p = p;
	this._sp = null;
    }
    function part(name, set) {
	return {
	    get: function () { return this._p[name]; },
	    set: function (v) {
		var q = Object.assign({}, this._p);
		set(q, String(v));
		var p = parseURL(compose(q), null);
		if (p) {
		    this._p = p;
		    if (this._sp) this._sp._l = new URLSearchParams(p.search)._l;
		}
	    },
	    enumerable: true, configurable: true
	};
    }
    Object.defineProperties(URL.prototype, {
	href: {
	    get: function () { return this._p.href; },
	    set: function (v) {
		var p = parseURL(v, null);
		if (!p) throw new TypeError("'" + v + "' is not a valid URL");
		this._p = p;
		if (this._sp) this._sp._l = new URLSearchParams(p.search)._l;
	    },
	    enumerable: true, configurable: true
	},
	origin: {
	    get: function () {
		var p = this._p;
		return DEFAULT_PORTS[p.protocol] !== undefined ? p.protocol + "//" + p.hostname + (p.port ? ":" + p.port : "") : "null";
	    },
	    enumerable: true, configurable: true
	},
	protocol: part("protocol", function (q, v) { q.protocol = v.replace(/:.*$/, "") + ":"; }),
	username: part("username", function (q, v) { q.username = encodeURIComponent(v); }),
	password: part("password", function (q, v) { q.password = encodeURIComponent(v); }),
	host: {
	    get: function () { return this._p.hostname + (this._p.port ? ":" + this._p.port : ""); },
	    set: part("host", function (q, v) {
		var m = /^([^:\/?#]*)(?::(\d*))?/.exec(v);
		q.hostname = m[1];
		if (m[2] !== undefined) q.port = m[2];
	    }).set,
	    enumerable: true, configurable: true
	},
	hostname: part("hostname", function (q, v) { q.hostname = v.replace(/[:\/?#].*$/, ""); }),
	port: part("port", function (q, v) { q.port = v.replace(/\D.*$/, ""); }),
	pathname: part("pathname", function (q, v) { q.pathname = (v.charAt(0) === "/" ? "" : "/") + v.replace(/[?#]/g, encodeURIComponent); }),
	search: part("search", function (q, v) { v = v.replace(/^\?/, ""); q.search = v ? "?" + v.replace(/#/g, "%23") : ""; }),
	hash: part("hash", function (q, v) { v = v.replace(/^#/, ""); q.hash = v ? "#" + v : ""; }),
	searchParams: {
	    get: function () {
		if (!this._sp) {
		    this._sp = new URLSearchParams(this._p.search);
		    this._sp._url = this;
		}
		return this._sp;
	    },
	    enumerable: true, configurable: true
	}
    });
    URL.prototype._setSearch = function (s) {
	var q = Object.assign({}, this._p);
	q.search = s ? "?" + s : "";
	var p = parseURL(compose(q), null);
	if (p) this._p = p;
    };
    URL.prototype.toString = URL.prototype.toJSON = function () { return this._p.href; };
    URL.canParse = function (url, base) {
	try { new URL(url, base); return true; } catch (e) { return false; }
    };
    URL.parse = function (url, base) {
	try { return new URL(url, base); } catch (e) { return null; }
    };
    /* object URLs: a data: URL of the blob (NetSurf fetches those) */
    URL.createObjectURL = function (blob) {
	if (!blob || typeof blob._nsDataURL !== "function") throw new TypeError("createObjectURL: not a Blob");
	return blob._nsDataURL();
    };
    URL.revokeObjectURL = function () {};
    define("URL", URL);

    /* a body for the primitive: a string (UTF-8) and the Content-Type it implies */
    function bodyOf(body) {
	if (body === undefined || body === null) return { text: null, type: null };
	if (typeof body === "string") return { text: body, type: "text/plain;charset=UTF-8" };
	if (body instanceof URLSearchParams) return { text: body.toString(), type: "application/x-www-form-urlencoded;charset=UTF-8" };
	if (typeof body._nsBody === "function") return body._nsBody();   /* FormData, Blob */
	if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) return { text: utf8decode(body), type: null };
	return { text: String(body), type: "text/plain;charset=UTF-8" };
    }

    /* XMLHttpRequest (asynchronous) */
    var XHR_EVENTS = ["readystatechange", "loadstart", "progress", "load", "error", "abort", "timeout", "loadend"];
    function XMLHttpRequest() {
	this._listeners = Object.create(null);
	this.readyState = 0;
	this.status = 0;
	this.statusText = "";
	this.responseURL = "";
	this.responseType = "";
	this.timeout = 0;
	this.withCredentials = false;
	this.upload = { addEventListener: function () {}, removeEventListener: function () {} };
	this._reset();
    }
    XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.HEADERS_RECEIVED = 2;
    XMLHttpRequest.LOADING = 3; XMLHttpRequest.DONE = 4;
    XMLHttpRequest.prototype = {
	UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4,
	_reset: function () {
	    this._id = 0; this._sent = false; this._rh = []; this._body = null;
	    this._text = ""; this.response = null; this.responseXML = null;
	},
	_fire: function (type, loaded, total) {
	    var e = { type: type, target: this, currentTarget: this, lengthComputable: total > 0,
		      loaded: loaded || 0, total: total || 0, preventDefault: function () {} };
	    var h = this["on" + type];
	    if (typeof h === "function") {
		try { h.call(this, e); } catch (x) { setTimeout(function () { throw x; }, 0); }
	    }
	    (this._listeners[type] || []).slice().forEach(function (f) {
		try { f.call(this, e); } catch (x) { setTimeout(function () { throw x; }, 0); }
	    }, this);
	},
	addEventListener: function (type, f) {
	    var l = this._listeners[type] || (this._listeners[type] = []);
	    if (typeof f === "function" && l.indexOf(f) < 0) l.push(f);
	},
	removeEventListener: function (type, f) {
	    var l = this._listeners[type];
	    if (l && l.indexOf(f) >= 0) l.splice(l.indexOf(f), 1);
	},
	dispatchEvent: function (e) { this._fire(e.type); return true; },
	_state: function (s) { this.readyState = s; this._fire("readystatechange"); },
	open: function (method, url, async) {
	    if (async === false) throw domError("InvalidAccessError", "synchronous XMLHttpRequest is not supported");
	    if (this._id) nsabort(this._id);
	    this._reset();
	    this._method = String(method).toUpperCase();
	    this._url = String(url);
	    this._headers = [];
	    this.status = 0; this.statusText = ""; this.responseURL = "";
	    this._state(1);
	},
	setRequestHeader: function (name, value) {
	    if (this.readyState !== 1 || this._sent) throw domError("InvalidStateError");
	    this._headers.push([String(name), String(value)]);
	},
	overrideMimeType: function () {},
	send: function (body) {
	    var xhr = this, b, hasType;
	    if (this.readyState !== 1 || this._sent) throw domError("InvalidStateError");
	    this._sent = true;
	    b = (this._method === "GET" || this._method === "HEAD") ? { text: null, type: null } : bodyOf(body);
	    hasType = this._headers.some(function (h) { return h[0].toLowerCase() === "content-type"; });
	    if (b.type && !hasType) this._headers.push(["Content-Type", b.type]);
	    this._fire("loadstart");
	    if (this.timeout > 0) {
		this._timer = setTimeout(function () {
		    if (xhr._id) { nsabort(xhr._id); xhr._id = 0; }
		    xhr._fail("timeout");
		}, this.timeout);
	    }
	    this._id = nsfetch(this._method, this._url, this._headers, b.text, function (kind, status, url, head, data) {
		xhr._id = 0;
		clearTimeout(xhr._timer);
		if (kind !== "done") { xhr._fail("error"); return; }
		xhr.status = status;
		xhr.statusText = STATUS[status] || "";
		xhr.responseURL = url;
		xhr._rh = parseHeaders(head);
		xhr._state(2);
		if (xhr._method === "HEAD") data = new ArrayBuffer(0);
		xhr._state(3);
		xhr._fire("progress", data.byteLength, data.byteLength);
		switch (xhr.responseType) {
		case "arraybuffer":
		case "blob":
		    xhr.response = data;
		    break;
		case "json":
		    try { xhr.response = JSON.parse(utf8decode(data)); } catch (e) { xhr.response = null; }
		    break;
		default:
		    xhr._text = utf8decode(data);
		    xhr.response = xhr._text;
		}
		xhr._state(4);
		xhr._fire("load", data.byteLength, data.byteLength);
		xhr._fire("loadend", data.byteLength, data.byteLength);
	    });
	},
	_fail: function (what) {
	    this.status = 0; this.statusText = ""; this.response = null; this._text = "";
	    this._state(4);
	    this._fire(what);
	    this._fire("loadend");
	},
	abort: function () {
	    if (this._id) { nsabort(this._id); this._id = 0; }
	    clearTimeout(this._timer);
	    if (this._sent && this.readyState !== 4) this._fail("abort");
	    this.readyState = 0;
	},
	get responseText() {
	    if (this.responseType !== "" && this.responseType !== "text") throw domError("InvalidStateError");
	    return this._text;
	},
	getResponseHeader: function (name) {
	    name = String(name).toLowerCase();
	    var v = this._rh.filter(function (h) { return h[0] === name; }).map(function (h) { return h[1]; });
	    return v.length ? v.join(", ") : null;
	},
	getAllResponseHeaders: function () {
	    return this._rh.map(function (h) { return h[0] + ": " + h[1] + "\r\n"; }).join("");
	}
    };
    XHR_EVENTS.forEach(function (t) { XMLHttpRequest.prototype["on" + t] = null; });
    define("XMLHttpRequest", XMLHttpRequest);

    /* Headers */
    function Headers(init) {
	this._l = [];
	if (init instanceof Headers) init = init._l;
	if (Array.isArray(init) || (init && typeof init[Symbol.iterator] === "function")) {
	    for (var p of init) this.append(p[0], p[1]);
	} else if (init && typeof init === "object") {
	    Object.keys(init).forEach(function (k) { this.append(k, init[k]); }, this);
	}
    }
    Headers.prototype = {
	append: function (k, v) { this._l.push([String(k).toLowerCase(), String(v)]); },
	set: function (k, v) { this.delete(k); this.append(k, v); },
	delete: function (k) { k = String(k).toLowerCase(); this._l = this._l.filter(function (p) { return p[0] !== k; }); },
	get: function (k) {
	    k = String(k).toLowerCase();
	    var v = this._l.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; });
	    return v.length ? v.join(", ") : null;
	},
	has: function (k) { return this.get(k) !== null; },
	forEach: function (f, t) { this._l.forEach(function (p) { f.call(t, p[1], p[0], this); }, this); },
	entries: function () { return this._l.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); },
	keys: function () { return this._l.map(function (p) { return p[0]; })[Symbol.iterator](); },
	values: function () { return this._l.map(function (p) { return p[1]; })[Symbol.iterator](); }
    };
    Headers.prototype[Symbol.iterator] = Headers.prototype.entries;
    define("Headers", Headers);

    /* Response */
    function Response(body, init) {
	init = init || {};
	var b = body === undefined || body === null ? null :
	    (body instanceof ArrayBuffer ? body : utf8encode(typeof body === "string" ? body : String(body)).buffer);
	this._buf = b;
	this.status = init.status === undefined ? 200 : init.status;
	this.statusText = init.statusText === undefined ? (STATUS[this.status] || "") : String(init.statusText);
	this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
	this.ok = this.status >= 200 && this.status < 300;
	this.url = "";
	this.redirected = false;
	this.type = "basic";
	this.bodyUsed = false;
    }
    Response.prototype = {
	_take: function () {
	    if (this.bodyUsed) return Promise.reject(new TypeError("the body has been read"));
	    this.bodyUsed = true;
	    return Promise.resolve(this._buf || new ArrayBuffer(0));
	},
	arrayBuffer: function () { return this._take(); },
	text: function () { return this._take().then(function (b) { return utf8decode(b); }); },
	json: function () { return this.text().then(function (t) { return JSON.parse(t); }); },
	clone: function () {
	    if (this.bodyUsed) throw new TypeError("the body has been read");
	    var r = new Response(this._buf ? this._buf.slice(0) : null, this);
	    r.url = this.url; r.redirected = this.redirected; r.type = this.type;
	    return r;
	}
    };
    Response.error = function () { var r = new Response(null, { status: 0 }); r.type = "error"; return r; };
    Response.json = function (data, init) {
	var h = new Headers((init && init.headers) || {});
	if (!h.has("content-type")) h.set("content-type", "application/json");
	return new Response(JSON.stringify(data), Object.assign({}, init, { headers: h }));
    };
    define("Response", Response);

    /* Request (what fetch needs of it) */
    function Request(input, init) {
	init = init || {};
	if (input instanceof Request) {
	    this.url = input.url; this.method = input.method;
	    this.headers = new Headers(input.headers); this._body = input._body;
	    this.signal = input.signal;
	} else {
	    this.url = String(input); this.method = "GET"; this.headers = new Headers(); this._body = null;
	    this.signal = null;
	}
	if (init.method) this.method = String(init.method).toUpperCase();
	if (init.headers) this.headers = new Headers(init.headers);
	if (init.body !== undefined) this._body = init.body;
	if (init.signal) this.signal = init.signal;
    }
    define("Request", Request);

    /* AbortController, AbortSignal */
    function AbortSignal() {
	this.aborted = false; this.reason = undefined; this.onabort = null; this._l = [];
    }
    AbortSignal.prototype = {
	addEventListener: function (t, f) { if (t === "abort" && typeof f === "function") this._l.push(f); },
	removeEventListener: function (t, f) { var i = this._l.indexOf(f); if (i >= 0) this._l.splice(i, 1); },
	throwIfAborted: function () { if (this.aborted) throw this.reason; },
	_abort: function (reason) {
	    if (this.aborted) return;
	    this.aborted = true;
	    this.reason = reason === undefined ? domError("AbortError", "aborted") : reason;
	    var e = { type: "abort", target: this };
	    if (typeof this.onabort === "function") this.onabort(e);
	    this._l.slice().forEach(function (f) { f.call(this, e); }, this);
	}
    };
    AbortSignal.abort = function (reason) { var s = new AbortSignal(); s._abort(reason); return s; };
    AbortSignal.timeout = function (ms) {
	var s = new AbortSignal();
	setTimeout(function () { s._abort(domError("TimeoutError", "timed out")); }, ms);
	return s;
    };
    function AbortController() { this.signal = new AbortSignal(); }
    AbortController.prototype.abort = function (reason) { this.signal._abort(reason); };
    define("AbortSignal", AbortSignal);
    define("AbortController", AbortController);

    /* fetch */
    define("fetch", function fetch(input, init) {
	var req = new Request(input, init);
	return new Promise(function (resolve, reject) {
	    var id, b, headers = [];
	    if (req.signal && req.signal.aborted) { reject(req.signal.reason); return; }
	    b = (req.method === "GET" || req.method === "HEAD") ? { text: null, type: null } : bodyOf(req._body);
	    req.headers.forEach(function (v, k) { headers.push([k, v]); });
	    if (b.type && !req.headers.has("content-type")) headers.push(["Content-Type", b.type]);
	    id = nsfetch(req.method, req.url, headers, b.text, function (kind, status, url, head, data, redirected) {
		id = 0;
		if (kind !== "done") {
		    reject(new TypeError("Failed to fetch (" + status + ")"));
		    return;
		}
		var r = new Response(req.method === "HEAD" || status === 204 || status === 304 ? null : data,
				     { status: status, headers: parseHeaders(head) });
		r.url = url;
		r.redirected = !!redirected;
		resolve(r);
	    });
	    if (req.signal) {
		req.signal.addEventListener("abort", function () {
		    if (id) { nsabort(id); id = 0; reject(req.signal.reason); }
		});
	    }
	});
    });
    return exports;
})
