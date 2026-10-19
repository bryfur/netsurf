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
 * ParentNode (selectors are selectors.js, the network net.js: made when
 * a page first names them),
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
    });

    /* Element */
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
