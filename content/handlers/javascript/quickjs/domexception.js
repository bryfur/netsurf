/*
 * netsurf-ng: DOMException (WebIDL), made the first time a page names it
 * or the bindings throw one (qjsky.c's modules).  An Error whose name is
 * the exception's and code the legacy number for the names that have one.
 *
 * The module: a function whose result is its exports.
 */
(function () {
    "use strict";
    var legacy = [
	["IndexSizeError", "INDEX_SIZE_ERR"], ["DOMStringSizeError", "DOMSTRING_SIZE_ERR"],
	["HierarchyRequestError", "HIERARCHY_REQUEST_ERR"], ["WrongDocumentError", "WRONG_DOCUMENT_ERR"],
	["InvalidCharacterError", "INVALID_CHARACTER_ERR"], ["NoDataAllowedError", "NO_DATA_ALLOWED_ERR"],
	["NoModificationAllowedError", "NO_MODIFICATION_ALLOWED_ERR"], ["NotFoundError", "NOT_FOUND_ERR"],
	["NotSupportedError", "NOT_SUPPORTED_ERR"], ["InUseAttributeError", "INUSE_ATTRIBUTE_ERR"],
	["InvalidStateError", "INVALID_STATE_ERR"], ["SyntaxError", "SYNTAX_ERR"],
	["InvalidModificationError", "INVALID_MODIFICATION_ERR"], ["NamespaceError", "NAMESPACE_ERR"],
	["InvalidAccessError", "INVALID_ACCESS_ERR"], ["ValidationError", "VALIDATION_ERR"],
	["TypeMismatchError", "TYPE_MISMATCH_ERR"], ["SecurityError", "SECURITY_ERR"],
	["NetworkError", "NETWORK_ERR"], ["AbortError", "ABORT_ERR"],
	["URLMismatchError", "URL_MISMATCH_ERR"], ["QuotaExceededError", "QUOTA_EXCEEDED_ERR"],
	["TimeoutError", "TIMEOUT_ERR"], ["InvalidNodeTypeError", "INVALID_NODE_TYPE_ERR"],
	["DataCloneError", "DATA_CLONE_ERR"]
    ];
    var codes = Object.create(null);

    class DOMException extends Error {
	constructor(message, name) {
	    super(message === undefined ? "" : String(message));
	    name = name === undefined ? "Error" : String(name);
	    Object.defineProperty(this, "name", { value: name, writable: true, configurable: true });
	    Object.defineProperty(this, "code", { value: codes[name] || 0, configurable: true });
	}
    }
    legacy.forEach(function (l, i) {
	var d = { value: i + 1, enumerable: true };
	codes[l[0]] = i + 1;
	Object.defineProperty(DOMException, l[1], d);
	Object.defineProperty(DOMException.prototype, l[1], d);
    });
    return { DOMException: DOMException };
})
