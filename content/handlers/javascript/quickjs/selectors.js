/*
 * netsurf-ng: selectors, made the first time a page uses them (qjsky.c's
 * modules): querySelector, querySelectorAll, matches, closest,
 * getElementsByClassName on Element, Document and DocumentFragment.
 *
 * Compound selectors (type, #id, .class, [attr op value i], pseudo-classes)
 * joined by the descendant, child, next-sibling and subsequent-sibling
 * combinators, groups by commas.  The pseudo-classes: structural
 * (:first-child ... :nth-last-of-type(), :empty, :root, :scope), logical
 * (:not() :is() :where() :matches() :has()), the form states (:checked
 * :disabled :enabled :required :optional :read-only :read-write
 * :placeholder-shown :valid :invalid), :link :any-link :target :focus
 * :focus-within :lang() :defined; the interaction states NetSurf does not
 * track (:hover :active :visited :focus-visible) match nothing, as do
 * pseudo-elements.  Anything else is a SyntaxError (DOMException), as in
 * browsers: libraries take that as their cue to use their own engine.
 *
 * The module: a function whose result is its exports.
 */
(function () {
    "use strict";
    var cache = Object.create(null);
    var scope = null;	/* :scope: the element a query is made from */

    /* the pseudo-classes known: those taking an argument, those not */
    var WITH_ARG = { "not": 1, "is": 1, "where": 1, "matches": 1, "has": 1, "lang": 1,
		     "nth-child": 1, "nth-last-child": 1, "nth-of-type": 1, "nth-last-of-type": 1 };
    var NO_ARG = { "first-child": 1, "last-child": 1, "only-child": 1, "first-of-type": 1,
		   "last-of-type": 1, "only-of-type": 1, "empty": 1, "root": 1, "scope": 1,
		   "checked": 1, "disabled": 1, "enabled": 1, "required": 1, "optional": 1,
		   "read-only": 1, "read-write": 1, "placeholder-shown": 1, "valid": 1,
		   "invalid": 1, "link": 1, "any-link": 1, "target": 1, "focus": 1,
		   "focus-within": 1, "defined": 1, "hover": 1, "active": 1, "visited": 1,
		   "focus-visible": 1,
		   /* CSS 2's pseudo-elements, single colon */
		   "before": 1, "after": 1, "first-line": 1, "first-letter": 1 };
    var NEVER = { "hover": 1, "active": 1, "visited": 1, "focus-visible": 1,
		  "before": 1, "after": 1, "first-line": 1, "first-letter": 1 };

    function syntaxError(text) {
	return new DOMException("'" + text + "' is not a valid selector", "SyntaxError");
    }

    function parseGroup(text) {
	if (cache[text]) return cache[text];
	var source = text, group = [], complex = [], compound = newCompound(), i = 0, m;
	function newCompound() {
	    return { tag: null, id: null, classes: [], attrs: [], pseudos: [], comb: " ", empty: true };
	}
	function flush(comb) {
	    if (compound.empty) throw syntaxError(source);
	    complex.push(compound);
	    compound = newCompound();
	    compound.comb = comb;
	}
	var re = {
	    ws: /^\s+/,
	    comb: /^\s*([>+~])\s*/,
	    comma: /^\s*,\s*/,
	    tag: /^(\*|[A-Za-z][\w-]*)/,
	    id: /^#((?:[\w-]|\\.)+)/,
	    cls: /^\.((?:[\w-]|\\.)+)/,
	    attr: /^\[\s*([\w-]+)\s*(?:([~|^$*]?=)\s*(?:"([^"]*)"|'([^']*)'|([^\]\s]+))\s*([iIsS])?\s*)?\]/,
	    pseudoElement: /^::([\w-]+)(?:\([^()]*\))?/,
	    pseudo: /^:([\w-]+)(?:\(((?:[^()]|\((?:[^()]|\([^()]*\))*\))*)\))?/
	};
	function unescape(s) { return s.replace(/\\(.)/g, "$1"); }
	text = text.trim();
	if (text === "") throw syntaxError(source);
	while (i < text.length) {
	    var rest = text.slice(i);
	    if ((m = re.comma.exec(rest))) {
		if (compound.empty) throw syntaxError(source);
		complex.push(compound); group.push(complex); complex = []; compound = newCompound();
		i += m[0].length;
		continue;
	    }
	    if ((m = re.comb.exec(rest))) { flush(m[1]); i += m[0].length; continue; }
	    if ((m = re.ws.exec(rest))) { flush(" "); i += m[0].length; continue; }
	    if ((m = re.tag.exec(rest))) {
		if (!compound.empty) throw syntaxError(source);
		compound.tag = m[1] === "*" ? null : m[1].toUpperCase();
	    } else if ((m = re.id.exec(rest))) {
		compound.id = unescape(m[1]);
	    } else if ((m = re.cls.exec(rest))) {
		compound.classes.push(unescape(m[1]));
	    } else if ((m = re.attr.exec(rest))) {
		var v = m[3] !== undefined ? m[3] : (m[4] !== undefined ? m[4] : m[5]);
		compound.attrs.push({ name: m[1], op: m[2] || null, value: v,
				      ci: !!m[6] && m[6].toLowerCase() === "i" });
	    } else if ((m = re.pseudoElement.exec(rest))) {
		compound.pseudos.push({ name: "before", arg: null });
	    } else if ((m = re.pseudo.exec(rest))) {
		var name = m[1].toLowerCase(), arg = m[2] === undefined ? null : m[2].trim();
		if (arg === null ? !NO_ARG[name] : !WITH_ARG[name]) throw syntaxError(source);
		var p = { name: name, arg: arg };
		if (name === "not" || name === "is" || name === "where" || name === "matches") {
		    p.group = parseGroup(arg);
		} else if (name === "has") {
		    /* relative selectors: ":scope" before each */
		    p.group = parseGroup(arg.split(",").map(function (r) {
			return ":scope " + r.trim();
		    }).join(","));
		} else if (name.indexOf("nth-") === 0) {
		    p.nth = parseNth(arg, source);
		}
		compound.pseudos.push(p);
	    } else {
		throw syntaxError(source);
	    }
	    compound.empty = false;
	    i += m[0].length;
	}
	if (compound.empty) throw syntaxError(source);
	complex.push(compound);
	group.push(complex);
	cache[source] = group;
	return group;
    }

    /* an+b: [a, b] */
    function parseNth(arg, source) {
	arg = arg.replace(/\s+/g, "").toLowerCase();
	if (arg === "odd") return [2, 1];
	if (arg === "even") return [2, 0];
	if (/^[+-]?\d+$/.test(arg)) return [0, parseInt(arg, 10)];
	var m = /^([+-]?\d*)n([+-]\d+)?$/.exec(arg);
	if (!m) throw syntaxError(source);
	var a = m[1] === "" || m[1] === "+" ? 1 : (m[1] === "-" ? -1 : parseInt(m[1], 10));
	return [a, m[2] ? parseInt(m[2], 10) : 0];
    }
    function nthMatch(nth, pos) {
	var a = nth[0], b = nth[1];
	return a === 0 ? pos === b : ((pos - b) / a >= 0 && (pos - b) % a === 0);
    }
    /* e's position among its element siblings (of its type), 1 first */
    function position(e, fromEnd, ofType) {
	var pos = 1, tag = e.tagName;
	for (var s = fromEnd ? e.nextElementSibling : e.previousElementSibling; s;
	     s = fromEnd ? s.nextElementSibling : s.previousElementSibling) {
	    if (!ofType || s.tagName === tag) pos++;
	}
	return pos;
    }

    var CONTROLS = { BUTTON: 1, INPUT: 1, SELECT: 1, TEXTAREA: 1, OPTGROUP: 1, OPTION: 1, FIELDSET: 1 };
    var REQUIRABLE = { INPUT: 1, SELECT: 1, TEXTAREA: 1 };
    function tag(e) { return e.tagName.toUpperCase(); }
    function writable(e) {
	var t = tag(e);
	if ((t === "INPUT" || t === "TEXTAREA") && !e.hasAttribute("readonly") &&
	    !e.hasAttribute("disabled")) return true;
	for (var n = e; n && n.nodeType === 1; n = n.parentNode) {
	    var ce = n.getAttribute("contenteditable");
	    if (ce !== null) return ce.toLowerCase() !== "false";
	}
	return false;
    }
    function active() {
	var d = scope ? scope.ownerDocument || scope : null;
	return d && d.activeElement ? d.activeElement : null;
    }

    function matchPseudo(e, p) {
	var t, a, n;
	if (NEVER[p.name]) return false;
	switch (p.name) {
	case "first-child": return position(e, false) === 1;
	case "last-child": return position(e, true) === 1;
	case "only-child": return position(e, false) === 1 && position(e, true) === 1;
	case "first-of-type": return position(e, false, true) === 1;
	case "last-of-type": return position(e, true, true) === 1;
	case "only-of-type": return position(e, false, true) === 1 && position(e, true, true) === 1;
	case "nth-child": return nthMatch(p.nth, position(e, false));
	case "nth-last-child": return nthMatch(p.nth, position(e, true));
	case "nth-of-type": return nthMatch(p.nth, position(e, false, true));
	case "nth-last-of-type": return nthMatch(p.nth, position(e, true, true));
	case "empty":
	    for (n = e.firstChild; n; n = n.nextSibling) {
		if (n.nodeType === 1 || (n.nodeType === 3 && n.data !== "")) return false;
	    }
	    return true;
	case "root": return e === e.ownerDocument.documentElement;
	case "scope": return scope && scope.nodeType === 1 ? e === scope : e === e.ownerDocument.documentElement;
	case "not": return !p.group.some(function (s) { return matchComplex(e, s); });
	case "is": case "where": case "matches":
	    return p.group.some(function (s) { return matchComplex(e, s); });
	case "has":
	    var saved = scope, all = descendants(e.parentNode || e), found = false;
	    scope = e;
	    try {
		for (var i = 0; i < all.length && !found; i++) {
		    found = all[i] !== e && p.group.some(function (s) { return matchComplex(all[i], s); });
		}
	    } finally {
		scope = saved;
	    }
	    return found;
	case "checked":
	    t = tag(e);
	    if (t === "INPUT") {
		return typeof e.checked === "boolean" ? e.checked : e.hasAttribute("checked");
	    }
	    if (t === "OPTION") {
		return typeof e.selected === "boolean" ? e.selected : e.hasAttribute("selected");
	    }
	    return false;
	case "disabled": return !!CONTROLS[tag(e)] && e.hasAttribute("disabled");
	case "enabled": return !!CONTROLS[tag(e)] && !e.hasAttribute("disabled");
	case "required": return !!REQUIRABLE[tag(e)] && e.hasAttribute("required");
	case "optional": return !!REQUIRABLE[tag(e)] && !e.hasAttribute("required");
	case "read-write": return writable(e);
	case "read-only": return !writable(e);
	case "placeholder-shown":
	    t = tag(e);
	    return (t === "INPUT" || t === "TEXTAREA") && e.hasAttribute("placeholder") &&
		(e.value === undefined ? true : e.value === "");
	case "valid": return !!REQUIRABLE[tag(e)] && !(e.hasAttribute("required") && e.value === "");
	case "invalid": return !!REQUIRABLE[tag(e)] && e.hasAttribute("required") && e.value === "";
	case "link": case "any-link":
	    t = tag(e);
	    return (t === "A" || t === "AREA") && e.hasAttribute("href");
	case "target":
	    a = typeof location !== "undefined" ? location.hash : "";
	    return a.length > 1 && e.id === decodeURIComponent(a.slice(1));
	case "focus": return e === active();
	case "focus-within":
	    for (n = active(); n; n = n.parentNode) {
		if (n === e) return true;
	    }
	    return false;
	case "lang":
	    var want = p.arg.replace(/^["']|["']$/g, "").toLowerCase();
	    for (n = e; n && n.nodeType === 1; n = n.parentNode) {
		var lang = n.getAttribute("lang");
		if (lang !== null) {
		    lang = lang.toLowerCase();
		    return lang === want || lang.indexOf(want + "-") === 0;
		}
	    }
	    return false;
	case "defined": return true;
	}
	return false;
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
	    if (!matchPseudo(e, c.pseudos[i])) return false;
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
    /* root's descendant elements in document order (no recursion) */
    function descendants(root) {
	var out = [], n = root.firstChild;
	while (n) {
	    if (n.nodeType === 1) out.push(n);
	    if (n.firstChild) {
		n = n.firstChild;
		continue;
	    }
	    while (n && n !== root && !n.nextSibling) n = n.parentNode;
	    if (!n || n === root) break;
	    n = n.nextSibling;
	}
	return out;
    }
    /* the group matched against elements with :scope set */
    function scoped(root, f) {
	var saved = scope;
	scope = root;
	try {
	    return f();
	} finally {
	    scope = saved;
	}
    }
    function query(root, sel, first) {
	var group = parseGroup(sel), out = [];
	/* an id alone: getElementById */
	if (group.length === 1 && group[0].length === 1 && root.getElementById) {
	    var c = group[0][0];
	    if (c.id !== null && !c.tag && !c.classes.length && !c.attrs.length && !c.pseudos.length) {
		var e = root.getElementById(c.id);
		out = e ? [e] : [];
		out.item = function (i) { return this[i] || null; };
		return out;
	    }
	}
	var all = descendants(root);
	scoped(root, function () {
	    for (var i = 0; i < all.length; i++) {
		if (group.some(function (s) { return matchComplex(all[i], s); })) {
		    out.push(all[i]);
		    if (first) break;
		}
	    }
	});
	out.item = function (i) { return this[i] || null; };
	return out;
    }

    function matches(sel) {
	var e = this, group = parseGroup(String(sel));
	return scoped(e, function () {
	    return group.some(function (s) { return matchComplex(e, s); });
	});
    }
    return {
	querySelector: function querySelector(sel) {
	    return query(this, String(sel), true)[0] || null;
	},
	querySelectorAll: function querySelectorAll(sel) {
	    return query(this, String(sel), false);
	},
	getElementsByClassName: function getElementsByClassName(names) {
	    var want = String(names).split(/\s+/).filter(Boolean);
	    return descendants(this).filter(function (e) {
		return want.every(function (c) { return e.classList.contains(c); });
	    });
	},
	matches: matches,
	webkitMatchesSelector: matches,
	closest: function closest(sel) {
	    var self = this, group = parseGroup(String(sel));
	    return scoped(self, function () {
		for (var e = self; e && e.nodeType === 1; e = e.parentNode) {
		    if (group.some(function (s) { return matchComplex(e, s); })) return e;
		}
		return null;
	    });
	}
    };
})
