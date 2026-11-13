#pragma once

#include <string>
#include <string_view>

/// Masking of credentials in the request and response dump that
/// ApostolModule::execute() writes to stderr in a _DEBUG build.
///
/// What is not recognised is not written, only its size: a body that is
/// neither JSON nor a form, a form part that is not name=value. In what is
/// recognised, values are masked by name (secret_name()), by shape (a bare
/// value, a value holding a query, pairs or JSON, a long path segment) and,
/// for headers, unless the header is on a list of plain ones.
namespace apostol::debug_mask
{

/// What a masked value is replaced with.
inline constexpr std::string_view masked = "<masked>";

/// True for a parameter, JSON key or header name whose value is a credential.
/// Case-insensitive.
bool secret_name(std::string_view name);

/// A query string or a form body (application/x-www-form-urlencoded):
/// "a=1&password=x" → "a=1&password=<masked>". A part without '=', with an
/// empty value or a value starting with '=' is masked whole (it may be a bare
/// credential with its base64 padding). Input that is
/// not a form — a name or a value with characters a form does not carry
/// unencoded (blanks, quotes, braces, control bytes …) — yields
/// "<masked> (<n> bytes, not a form)".
std::string mask_form(std::string_view s);

/// A URL path: a segment of 32 characters or more is masked, unless it is
/// a UUID.
std::string mask_path(std::string_view path);

/// A request or response body. A JSON object or array is masked by name at any
/// depth, strings inside arrays and strings holding JSON whole; a JSON string
/// or number on its own is masked whole; anything else is
/// read as a form (mask_form), so a body that is neither — broken JSON
/// included — is not written.
std::string mask_body(std::string_view body);

/// A header value: the scheme of Authorization stays, the rest is masked; a
/// cookie keeps its name; a URL-valued header (Referer, Location,
/// Content-Location) has its userinfo masked, its path through mask_path, its
/// query and fragment through mask_form; a header on the plain list is
/// written as is; any other header value is masked.
std::string mask_header(std::string_view name, std::string_view value);

} // namespace apostol::debug_mask
