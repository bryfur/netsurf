/* Polyfiller for Duktape for NetSurf
 *
 * This JavaScript will be loaded into heaps before the generics
 *
 * We only care for the side-effects of this, be careful.
 */

// Production steps of ECMA-262, Edition 6, 22.1.2.1
if (!Array.from) {
  Array.from = (function () {
    var toStr = Object.prototype.toString;
    var isCallable = function (fn) {
      return typeof fn === 'function' || toStr.call(fn) === '[object Function]';
    };
    var toInteger = function (value) {
      var number = Number(value);
      if (isNaN(number)) { return 0; }
      if (number === 0 || !isFinite(number)) { return number; }
      return (number > 0 ? 1 : -1) * Math.floor(Math.abs(number));
    };
    var maxSafeInteger = Math.pow(2, 53) - 1;
    var toLength = function (value) {
      var len = toInteger(value);
      return Math.min(Math.max(len, 0), maxSafeInteger);
    };

    // The length property of the from method is 1.
    return function from(arrayLike/*, mapFn, thisArg */) {
      // 1. Let C be the this value.
      var C = this;

      // 2. Let items be ToObject(arrayLike).
      var items = Object(arrayLike);

      // 3. ReturnIfAbrupt(items).
      if (arrayLike == null) {
        throw new TypeError('Array.from requires an array-like object - not null or undefined');
      }

      // 4. If mapfn is undefined, then let mapping be false.
      var mapFn = arguments.length > 1 ? arguments[1] : void undefined;
      var T;
      if (typeof mapFn !== 'undefined') {
        // 5. else
        // 5. a If IsCallable(mapfn) is false, throw a TypeError exception.
        if (!isCallable(mapFn)) {
          throw new TypeError('Array.from: when provided, the second argument must be a function');
        }

        // 5. b. If thisArg was supplied, let T be thisArg; else let T be undefined.
        if (arguments.length > 2) {
          T = arguments[2];
        }
      }

      // 10. Let lenValue be Get(items, "length").
      // 11. Let len be ToLength(lenValue).
      var len = toLength(items.length);

      // 13. If IsConstructor(C) is true, then
      // 13. a. Let A be the result of calling the [[Construct]] internal method 
      // of C with an argument list containing the single item len.
      // 14. a. Else, Let A be ArrayCreate(len).
      var A = isCallable(C) ? Object(new C(len)) : new Array(len);

      // 16. Let k be 0.
      var k = 0;
      // 17. Repeat, while k < len… (also steps a - h)
      var kValue;
      while (k < len) {
        kValue = items[k];
        if (mapFn) {
          A[k] = typeof T === 'undefined' ? mapFn(kValue, k) : mapFn.call(T, kValue, k);
        } else {
          A[k] = kValue;
        }
        k += 1;
      }
      // 18. Let putStatus be Put(A, "length", len, true).
      A.length = len;
      // 20. Return A.
      return A;
    };
  }());
}

// DOMTokenList formatter, in theory we can remove this if we do the stringifier IDL support

DOMTokenList.prototype.toString = function () {
  if (this.length == 0) {
    return "";
  }

  var ret = this.item(0);
  for (var index = 1; index < this.length; index++) {
    ret = ret + " " + this.item(index);
  }

  return ret;
}

// Inherit the same toString for settable lists
DOMSettableTokenList.prototype.toString = DOMTokenList.prototype.toString;
/* netsurf-ng: the lists as arrays are: forEach, for...of */
[NodeList, HTMLCollection, NamedNodeMap, DOMTokenList].forEach(function (I) {
    I.prototype.forEach = Array.prototype.forEach;
    I.prototype[Symbol.iterator] = function* () {
	for (var i = 0; i < this.length; i++) {
	    yield this.item(i);
	}
    };
});

/*
 * netsurf-ng: DOM the bindings do not have natively yet.  ChildNode and
 * ParentNode, selectors (querySelector, matches, closest), class lookup,
 * dataset, hidden, atob/btoa, Web Storage (per page, in memory), the Event
 * and CustomEvent constructors, small pieces of the window.
 */
(function () {
    "use strict";

    function method(proto, name, fn) {
	if (!(name in proto)) {
	    Object.defineProperty(proto, name, { value: fn, writable: true, configurable: true });
	}
    }
    function getter(proto, name, get, set) {
	Object.defineProperty(proto, name, { get: get, set: set, configurable: true, enumerable: true });
    }
    function toNode(doc, v) {
	return (v instanceof Node) ? v : doc.createTextNode(String(v));
    }
    function nodesOf(doc, args) {
	if (args.length === 1) {
	    return toNode(doc, args[0]);
	}
	var frag = doc.createDocumentFragment();
	for (var i = 0; i < args.length; i++) {
	    frag.appendChild(toNode(doc, args[i]));
	}
	return frag;
    }

    /* ChildNode */
    [Element.prototype, CharacterData.prototype].forEach(function (p) {
	method(p, "remove", function () {
	    if (this.parentNode) this.parentNode.removeChild(this);
	});
	method(p, "before", function () {
	    var parent = this.parentNode;
	    if (parent) parent.insertBefore(nodesOf(this.ownerDocument, arguments), this);
	});
	method(p, "after", function () {
	    var parent = this.parentNode;
	    if (parent) parent.insertBefore(nodesOf(this.ownerDocument, arguments), this.nextSibling);
	});
	method(p, "replaceWith", function () {
	    var parent = this.parentNode;
	    if (parent) parent.replaceChild(nodesOf(this.ownerDocument, arguments), this);
	});
    });

    /* ParentNode */
    function elementChildren() {
	var out = [];
	for (var n = this.firstChild; n; n = n.nextSibling) {
	    if (n.nodeType === 1) out.push(n);
	}
	return out;
    }
    [Element.prototype, Document.prototype, DocumentFragment.prototype].forEach(function (p) {
	if (!Object.getOwnPropertyDescriptor(p, "children")) getter(p, "children", elementChildren);
	method(p, "append", function () {
	    this.appendChild(nodesOf(this.ownerDocument || this, arguments));
	});
	method(p, "prepend", function () {
	    this.insertBefore(nodesOf(this.ownerDocument || this, arguments), this.firstChild);
	});
	method(p, "querySelector", function (sel) {
	    return query(this, String(sel), true)[0] || null;
	});
	method(p, "querySelectorAll", function (sel) {
	    return query(this, String(sel), false);
	});
	method(p, "getElementsByClassName", function (names) {
	    var want = String(names).split(/\s+/).filter(Boolean);
	    return descendants(this).filter(function (e) {
		return want.every(function (c) { return e.classList.contains(c); });
	    });
	});
    });

    /* Element */
    method(Element.prototype, "matches", function (sel) {
	return parseGroup(String(sel)).some(function (s) { return matchComplex(this, s); }, this);
    });
    method(Element.prototype, "webkitMatchesSelector", Element.prototype.matches);
    method(Element.prototype, "closest", function (sel) {
	var group = parseGroup(String(sel));
	for (var e = this; e && e.nodeType === 1; e = e.parentNode) {
	    if (group.some(function (s) { return matchComplex(e, s); })) return e;
	}
	return null;
    });
    method(Element.prototype, "toggleAttribute", function (name, force) {
	var has = this.hasAttribute(name);
	if (force === undefined) force = !has;
	if (force && !has) this.setAttribute(name, "");
	if (!force && has) this.removeAttribute(name);
	return !!force;
    });
    method(Element.prototype, "hasAttributes", function () { return this.attributes.length > 0; });
    method(Element.prototype, "getAttributeNames", function () {
	return Array.from(this.attributes, function (a) { return a.name; });
    });
    method(Element.prototype, "insertAdjacentHTML", function (where, html) {
	var t = this.ownerDocument.createElement("div");
	t.innerHTML = html;
	var frag = this.ownerDocument.createDocumentFragment();
	while (t.firstChild) frag.appendChild(t.firstChild);
	switch (String(where).toLowerCase()) {
	case "beforebegin": this.parentNode.insertBefore(frag, this); break;
	case "afterbegin": this.insertBefore(frag, this.firstChild); break;
	case "beforeend": this.appendChild(frag); break;
	case "afterend": this.parentNode.insertBefore(frag, this.nextSibling); break;
	default: throw new SyntaxError("insertAdjacentHTML: " + where);
	}
    });

    /* Node */
    getter(Node.prototype, "isConnected", function () {
	for (var n = this; n; n = n.parentNode) {
	    if (n.nodeType === 9) return true;
	}
	return false;
    });
    method(Node.prototype, "getRootNode", function () {
	var n = this;
	while (n.parentNode) n = n.parentNode;
	return n;
    });

    /* HTMLElement */
    getter(HTMLElement.prototype, "hidden",
	   function () { return this.hasAttribute("hidden"); },
	   function (v) { this.toggleAttribute("hidden", !!v); });
    function camel(s) { return s.replace(/-([a-z])/g, function (_, c) { return c.toUpperCase(); }); }
    function dashed(s) { return s.replace(/[A-Z]/g, function (c) { return "-" + c.toLowerCase(); }); }
    getter(HTMLElement.prototype, "dataset", function () {
	var el = this;
	return new Proxy({}, {
	    get: function (t, k) {
		if (typeof k !== "string") return undefined;
		var v = el.getAttribute("data-" + dashed(k));
		return v === null ? undefined : v;
	    },
	    set: function (t, k, v) { el.setAttribute("data-" + dashed(String(k)), String(v)); return true; },
	    has: function (t, k) { return typeof k === "string" && el.hasAttribute("data-" + dashed(k)); },
	    deleteProperty: function (t, k) { el.removeAttribute("data-" + dashed(String(k))); return true; },
	    ownKeys: function () {
		return el.getAttributeNames().filter(function (n) { return n.indexOf("data-") === 0; })
		    .map(function (n) { return camel(n.slice(5)); });
	    },
	    getOwnPropertyDescriptor: function (t, k) {
		var v = el.getAttribute("data-" + dashed(String(k)));
		return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true };
	    }
	});
    });
    method(HTMLElement.prototype, "click", function () {
	var e = document.createEvent("Event");
	e.initEvent("click", true, true);
	this.dispatchEvent(e);
    });
    method(HTMLElement.prototype, "focus", function () {});
    method(HTMLElement.prototype, "blur", function () {});

    /* Selectors: compound selectors (type, #id, .class, [attr op value],
     * :first-child :last-child :only-child :nth-child() :nth-last-child()
     * :not() :checked :disabled :enabled :empty :root) joined by the
     * descendant, child, next-sibling and subsequent-sibling combinators,
     * groups by commas
     */
    var cache = Object.create(null);
    function parseGroup(text) {
	if (cache[text]) return cache[text];
	var group = [], complex = [], compound = newCompound(), i = 0, m;
	function newCompound() { return { tag: null, id: null, classes: [], attrs: [], pseudos: [], comb: " " }; }
	function flush(comb) {
	    complex.push(compound);
	    compound = newCompound();
	    compound.comb = comb;
	}
	var re = {
	    ws: /^\s+/,
	    comb: /^\s*([>+~])\s*/,
	    comma: /^\s*,\s*/,
	    tag: /^(\*|[A-Za-z][\w-]*)/,
	    id: /^#([\w-]+)/,
	    cls: /^\.([\w-]+)/,
	    attr: /^\[\s*([\w-]+)\s*(?:([~|^$*]?=)\s*(?:"([^"]*)"|'([^']*)'|([^\]\s]+))\s*(i)?\s*)?\]/,
	    pseudo: /^:([\w-]+)(?:\(([^()]*(?:\([^()]*\))?[^()]*)\))?/
	};
	text = text.trim();
	while (i < text.length) {
	    var rest = text.slice(i);
	    if ((m = re.comma.exec(rest))) { complex.push(compound); group.push(complex); complex = []; compound = newCompound(); }
	    else if ((m = re.comb.exec(rest))) { flush(m[1]); }
	    else if ((m = re.ws.exec(rest))) { flush(" "); }
	    else if ((m = re.tag.exec(rest))) { compound.tag = m[1] === "*" ? null : m[1].toUpperCase(); }
	    else if ((m = re.id.exec(rest))) { compound.id = m[1]; }
	    else if ((m = re.cls.exec(rest))) { compound.classes.push(m[1]); }
	    else if ((m = re.attr.exec(rest))) {
		var v = m[3] !== undefined ? m[3] : (m[4] !== undefined ? m[4] : m[5]);
		compound.attrs.push({ name: m[1], op: m[2] || null, value: v, ci: !!m[6] });
	    }
	    else if ((m = re.pseudo.exec(rest))) {
		compound.pseudos.push({ name: m[1].toLowerCase(), arg: m[2] === undefined ? null : m[2].trim() });
	    }
	    else { throw new SyntaxError("'" + text + "' is not a valid selector"); }
	    i += m[0].length;
	}
	complex.push(compound);
	group.push(complex);
	cache[text] = group;
	return group;
    }
    function nthMatch(arg, pos) {
	arg = arg.replace(/\s+/g, "").toLowerCase();
	if (arg === "odd") return pos % 2 === 1;
	if (arg === "even") return pos % 2 === 0;
	var m = /^([+-]?\d*)n([+-]\d+)?$/.exec(arg);
	if (!m) return pos === parseInt(arg, 10);
	var a = m[1] === "" || m[1] === "+" ? 1 : (m[1] === "-" ? -1 : parseInt(m[1], 10));
	var b = m[2] ? parseInt(m[2], 10) : 0;
	return a === 0 ? pos === b : ((pos - b) / a >= 0 && (pos - b) % a === 0);
    }
    function position(e, fromEnd) {
	var pos = 1;
	for (var s = fromEnd ? e.nextSibling : e.previousSibling; s; s = fromEnd ? s.nextSibling : s.previousSibling) {
	    if (s.nodeType === 1) pos++;
	}
	return pos;
    }
    function matchCompound(e, c) {
	if (c.tag !== null && e.tagName.toUpperCase() !== c.tag) return false;
	if (c.id !== null && e.id !== c.id) return false;
	for (var i = 0; i < c.classes.length; i++) {
	    if (!e.classList.contains(c.classes[i])) return false;
	}
	for (i = 0; i < c.attrs.length; i++) {
	    var a = c.attrs[i], v = e.getAttribute(a.name);
	    if (v === null) return false;
	    if (a.op === null) continue;
	    var want = a.value;
	    if (a.ci) { v = v.toLowerCase(); want = want.toLowerCase(); }
	    switch (a.op) {
	    case "=": if (v !== want) return false; break;
	    case "~=": if (v.split(/\s+/).indexOf(want) < 0) return false; break;
	    case "|=": if (v !== want && v.indexOf(want + "-") !== 0) return false; break;
	    case "^=": if (!want || v.indexOf(want) !== 0) return false; break;
	    case "$=": if (!want || v.slice(-want.length) !== want) return false; break;
	    case "*=": if (!want || v.indexOf(want) < 0) return false; break;
	    }
	}
	for (i = 0; i < c.pseudos.length; i++) {
	    var p = c.pseudos[i];
	    switch (p.name) {
	    case "first-child": if (position(e, false) !== 1) return false; break;
	    case "last-child": if (position(e, true) !== 1) return false; break;
	    case "only-child": if (position(e, false) !== 1 || position(e, true) !== 1) return false; break;
	    case "nth-child": if (!nthMatch(p.arg, position(e, false))) return false; break;
	    case "nth-last-child": if (!nthMatch(p.arg, position(e, true))) return false; break;
	    case "not":
		if (parseGroup(p.arg).some(function (s) { return matchComplex(e, s); })) return false;
		break;
	    case "checked": if (!e.hasAttribute("checked") && !e.hasAttribute("selected")) return false; break;
	    case "disabled": if (!e.hasAttribute("disabled")) return false; break;
	    case "enabled": if (e.hasAttribute("disabled")) return false; break;
	    case "empty": if (e.firstChild) return false; break;
	    case "root": if (e !== e.ownerDocument.documentElement) return false; break;
	    default: return false;
	    }
	}
	return true;
    }
    /* right to left */
    function matchComplex(e, complex) {
	function at(e, k) {
	    if (!matchCompound(e, complex[k])) return false;
	    if (k === 0) return true;
	    var comb = complex[k].comb, n;
	    switch (comb) {
	    case ">": n = e.parentNode; return n && n.nodeType === 1 && at(n, k - 1);
	    case "+": n = e.previousElementSibling; return n && at(n, k - 1);
	    case "~":
		for (n = e.previousElementSibling; n; n = n.previousElementSibling) {
		    if (at(n, k - 1)) return true;
		}
		return false;
	    default:
		for (n = e.parentNode; n && n.nodeType === 1; n = n.parentNode) {
		    if (at(n, k - 1)) return true;
		}
		return false;
	    }
	}
	return at(e, complex.length - 1);
    }
    function descendants(root) {
	var out = [];
	(function walk(n) {
	    for (var c = n.firstChild; c; c = c.nextSibling) {
		if (c.nodeType === 1) {
		    out.push(c);
		    walk(c);
		}
	    }
	})(root);
	return out;
    }
    function query(root, sel, first) {
	var group = parseGroup(sel), out = [];
	/* an id alone: getElementById */
	if (group.length === 1 && group[0].length === 1 && root.getElementById) {
	    var c = group[0][0];
	    if (c.id !== null && !c.tag && !c.classes.length && !c.attrs.length && !c.pseudos.length) {
		var e = root.getElementById(c.id);
		return e ? [e] : [];
	    }
	}
	var all = descendants(root);
	for (var i = 0; i < all.length; i++) {
	    if (group.some(function (s) { return matchComplex(all[i], s); })) {
		out.push(all[i]);
		if (first) break;
	    }
	}
	out.item = function (i) { return this[i] || null; };
	return out;
    }

    /* the window */
    var g = globalThis;
    /* a fallback for something the window lacks (read only attributes
     * included: an own property shadows the prototype's)
     */
    function fallback(name, value) {
	if (g[name] === undefined) {
	    Object.defineProperty(g, name, { value: value, writable: true, configurable: true });
	}
    }
    var b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    function invalidChar(what) {
	var e = new Error(what);
	e.name = "InvalidCharacterError";
	e.code = 5;
	return e;
    }
    fallback("btoa", function (s) {
	s = String(s);
	var out = "";
	for (var i = 0; i < s.length; i += 3) {
	    var a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
	    if (a > 255 || b > 255 || c > 255) throw invalidChar("btoa: not Latin-1");
	    var n = (a << 16) | ((b || 0) << 8) | (c || 0);
	    out += b64[n >> 18] + b64[(n >> 12) & 63] +
		(i + 1 < s.length ? b64[(n >> 6) & 63] : "=") +
		(i + 2 < s.length ? b64[n & 63] : "=");
	}
	return out;
    });
    fallback("atob", function (s) {
	s = String(s).replace(/[\t\n\f\r ]/g, "");
	if (s.length % 4 === 0) s = s.replace(/==?$/, "");
	if (s.length % 4 === 1 || /[^A-Za-z0-9+\/]/.test(s)) throw invalidChar("atob: not base64");
	var out = "", bits = 0, n = 0;
	for (var i = 0; i < s.length; i++) {
	    n = (n << 6) | b64.indexOf(s[i]);
	    bits += 6;
	    if (bits >= 8) {
		bits -= 8;
		out += String.fromCharCode((n >> bits) & 255);
	    }
	}
	return out;
    });

    /* Web Storage: this page's, in memory */
    function Storage() {}
    Storage.prototype = {
	getItem: function (k) { k = String(k); return Object.prototype.hasOwnProperty.call(this._d, k) ? this._d[k] : null; },
	setItem: function (k, v) { this._d[String(k)] = String(v); },
	removeItem: function (k) { delete this._d[String(k)]; },
	clear: function () { this._d = Object.create(null); },
	key: function (i) { var k = Object.keys(this._d); return i < k.length ? k[i] : null; },
	get length() { return Object.keys(this._d).length; }
    };
    function makeStorage() {
	var s = new Storage();
	Object.defineProperty(s, "_d", { value: Object.create(null), writable: true });
	return new Proxy(s, {
	    get: function (t, k) { return (k in t || typeof k !== "string") ? t[k] : t.getItem(k) === null ? undefined : t.getItem(k); },
	    set: function (t, k, v) { if (k in t) t[k] = v; else t.setItem(k, v); return true; },
	    deleteProperty: function (t, k) { t.removeItem(k); return true; },
	    has: function (t, k) { return k in t || t.getItem(k) !== null; },
	    ownKeys: function (t) { return Object.keys(t._d); },
	    getOwnPropertyDescriptor: function (t, k) {
		var v = t.getItem(k);
		return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true };
	    }
	});
    }
    Object.defineProperty(g, "Storage", { value: Storage, writable: true, configurable: true });
    var local, session;
    Object.defineProperty(g, "localStorage", { get: function () { return local || (local = makeStorage()); }, configurable: true });
    Object.defineProperty(g, "sessionStorage", { get: function () { return session || (session = makeStorage()); }, configurable: true });

    /* constructors for events */
    function construct(name, init, detail) {
	var e = document.createEvent("Event");
	e.initEvent(String(name), !!(init && init.bubbles), !!(init && init.cancelable));
	if (detail) {
	    Object.defineProperty(e, "detail", { value: init && init.detail !== undefined ? init.detail : null, enumerable: true });
	}
	return e;
    }
    var EventI = g.Event;
    function EventCtor(type, init) {
	if (!new.target) throw new TypeError("Event: use new");
	if (arguments.length < 1) throw new TypeError("Event: 1 argument required");
	return construct(type, init, false);
    }
    function CustomEventCtor(type, init) {
	if (!new.target) throw new TypeError("CustomEvent: use new");
	if (arguments.length < 1) throw new TypeError("CustomEvent: 1 argument required");
	return construct(type, init, true);
    }
    EventCtor.prototype = EventI.prototype;
    CustomEventCtor.prototype = EventI.prototype;
    ["NONE", "CAPTURING_PHASE", "AT_TARGET", "BUBBLING_PHASE"].forEach(function (k) {
	if (k in EventI) EventCtor[k] = EventI[k];
    });
    Object.defineProperty(EventI.prototype, "constructor", { value: EventCtor, writable: true, configurable: true });
    Object.defineProperty(g, "Event", { value: EventCtor, writable: true, configurable: true });
    Object.defineProperty(g, "CustomEvent", { value: CustomEventCtor, writable: true, configurable: true });

    fallback("matchMedia", function (q) {
	return { matches: false, media: String(q), onchange: null,
		 addListener: function () {}, removeListener: function () {},
		 addEventListener: function () {}, removeEventListener: function () {},
		 dispatchEvent: function () { return false; } };
    });
    fallback("history", {
	length: 1, state: null, scrollRestoration: "auto",
	pushState: function (s) { this.state = s; }, replaceState: function (s) { this.state = s; },
	back: function () {}, forward: function () {}, go: function () {}
    });
    fallback("getComputedStyle", function (e) { return e.style; });
    if (typeof CSSStyleDeclaration !== "undefined") {
	method(CSSStyleDeclaration.prototype, "getPropertyValue", function (n) { var v = this[camel(String(n))]; return v === undefined ? "" : String(v); });
	method(CSSStyleDeclaration.prototype, "setProperty", function (n, v) { this[camel(String(n))] = v === null ? "" : String(v); });
	method(CSSStyleDeclaration.prototype, "removeProperty", function (n) { var k = camel(String(n)), v = this[k]; delete this[k]; return v === undefined ? "" : v; });
    }
    var t0 = Date.now();
    fallback("performance", { now: function () { return Date.now() - t0; }, timeOrigin: t0 });
    fallback("queueMicrotask", function (f) { Promise.resolve().then(f); });
})();

/*
 * netsurf-ng: the network from scripts.  XMLHttpRequest and fetch() over
 * the engine's primitive (__nsfetch: NetSurf's fetchers, the same-origin
 * and simple-CORS rules), Headers, Request, Response, AbortController,
 * TextEncoder and TextDecoder (UTF-8), URLSearchParams.
 */
(function () {
    "use strict";
    var g = globalThis;
    var nsfetch = g.__nsfetch, nsabort = g.__nsfetch_abort;
    var utf8decode = g.__nsutf8decode, utf8encode = g.__nsutf8encode;
    ["__nsfetch", "__nsfetch_abort", "__nsutf8decode", "__nsutf8encode"].forEach(function (k) { delete g[k]; });

    function define(name, value) {
	Object.defineProperty(g, name, { value: value, writable: true, configurable: true });
    }
    function domError(name, message) {
	var e = new Error(message || name);
	e.name = name;
	return e;
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
	append: function (k, v) { this._l.push([String(k), String(v)]); },
	delete: function (k) { k = String(k); this._l = this._l.filter(function (p) { return p[0] !== k; }); },
	get: function (k) { k = String(k); for (var p of this._l) if (p[0] === k) return p[1]; return null; },
	getAll: function (k) { k = String(k); return this._l.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); },
	has: function (k) { return this.get(k) !== null; },
	set: function (k, v) {
	    k = String(k); v = String(v);
	    var i = this._l.findIndex(function (p) { return p[0] === k; });
	    if (i < 0) { this._l.push([k, v]); return; }
	    this._l[i][1] = v;
	    this._l = this._l.filter(function (p, j) { return j <= i || p[0] !== k; });
	},
	sort: function () { this._l.sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }); },
	toString: function () { return this._l.map(function (p) { return encodeForm(p[0]) + "=" + encodeForm(p[1]); }).join("&"); },
	forEach: function (f, t) { this._l.forEach(function (p) { f.call(t, p[1], p[0], this); }, this); },
	entries: function () { return this._l.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); },
	keys: function () { return this._l.map(function (p) { return p[0]; })[Symbol.iterator](); },
	values: function () { return this._l.map(function (p) { return p[1]; })[Symbol.iterator](); },
	get size() { return this._l.length; }
    };
    URLSearchParams.prototype[Symbol.iterator] = URLSearchParams.prototype.entries;
    define("URLSearchParams", URLSearchParams);

    /* a body for the primitive: a string (UTF-8) and the Content-Type it implies */
    function bodyOf(body) {
	if (body === undefined || body === null) return { text: null, type: null };
	if (typeof body === "string") return { text: body, type: "text/plain;charset=UTF-8" };
	if (body instanceof URLSearchParams) return { text: body.toString(), type: "application/x-www-form-urlencoded;charset=UTF-8" };
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
})();
