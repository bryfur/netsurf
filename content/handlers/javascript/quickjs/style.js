/*
 * netsurf-ng: element.style, a CSSStyleDeclaration over the element's style
 * attribute, made the first time a page asks for an element's style
 * (qjsky.c's modules).  What is set is written into the attribute, which
 * NetSurf styles the element by (the page laid out again); what is read
 * comes from it.  Properties by their CSS names (setProperty,
 * getPropertyValue, removeProperty) or as camelCase ones (style.marginTop,
 * style.cssFloat, style.webkitTransform).
 *
 * The module: a function whose result is its exports.
 */
(function () {
    "use strict";

    function cssName(p) {
	if (p === "cssFloat") {
	    return "float";
	}
	p = p.replace(/[A-Z]/g, function (c) { return "-" + c.toLowerCase(); });
	return /^(webkit|moz|ms|o)-/.test(p) ? "-" + p : p;
    }

    function parse(text) {
	var m = new Map();
	String(text || "").split(";").forEach(function (d) {
	    var i = d.indexOf(":");
	    if (i > 0) {
		var k = d.slice(0, i).trim().toLowerCase();
		var v = d.slice(i + 1).trim();
		if (k !== "") {
		    m.set(k, v);
		}
	    }
	});
	return m;
    }

    function declaration(el) {
	function decls() {
	    return parse(el.getAttribute("style"));
	}
	function write(m) {
	    var s = [];
	    m.forEach(function (v, k) { s.push(k + ": " + v + ";"); });
	    if (s.length > 0) {
		el.setAttribute("style", s.join(" "));
	    } else if (el.hasAttribute("style")) {
		el.removeAttribute("style");
	    }
	}
	var api = {
	    get cssText() { return el.getAttribute("style") || ""; },
	    set cssText(t) { el.setAttribute("style", String(t)); },
	    get length() { return decls().size; },
	    get parentRule() { return null; },
	    item: function (i) { return Array.from(decls().keys())[i] || ""; },
	    getPropertyValue: function (p) {
		return decls().get(String(p).toLowerCase()) || "";
	    },
	    getPropertyPriority: function () { return ""; },
	    setProperty: function (p, v) {
		var m = decls();
		p = String(p).toLowerCase();
		if (v === null || v === undefined || String(v) === "") {
		    m.delete(p);
		} else {
		    m.set(p, String(v));
		}
		write(m);
	    },
	    removeProperty: function (p) {
		var m = decls(), old;
		p = String(p).toLowerCase();
		old = m.get(p) || "";
		m.delete(p);
		write(m);
		return old;
	    }
	};
	return new Proxy(api, {
	    get: function (t, p) {
		if (typeof p !== "string" || p in t) {
		    return Reflect.get(t, p);
		}
		return /^[0-9]+$/.test(p) ? t.item(+p) : t.getPropertyValue(cssName(p));
	    },
	    set: function (t, p, v) {
		if (typeof p !== "string" || p in t) {
		    return Reflect.set(t, p, v);
		}
		t.setProperty(cssName(p), v);
		return true;
	    },
	    has: function (t, p) {
		return typeof p !== "string" || p in t || t.getPropertyValue(cssName(p)) !== "";
	    }
	});
    }

    return { declaration: declaration };
})
