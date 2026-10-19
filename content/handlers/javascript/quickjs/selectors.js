/*
 * netsurf-ng: selectors, made the first time a page uses them (qjsky.c's
 * modules): querySelector, querySelectorAll, matches, closest,
 * getElementsByClassName on Element, Document and DocumentFragment.
 * Compound selectors (type, #id, .class, [attr op value], :first-child
 * :last-child :only-child :nth-child() :nth-last-child() :not() :checked
 * :disabled :enabled :empty :root) joined by the descendant, child,
 * next-sibling and subsequent-sibling combinators, groups by commas.
 *
 * The module: a function whose result is its exports.
 */
(function () {
    "use strict";
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

    function matches(sel) {
	return parseGroup(String(sel)).some(function (s) { return matchComplex(this, s); }, this);
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
	    var group = parseGroup(String(sel));
	    for (var e = this; e && e.nodeType === 1; e = e.parentNode) {
		if (group.some(function (s) { return matchComplex(e, s); })) return e;
	    }
	    return null;
	}
    };
})
