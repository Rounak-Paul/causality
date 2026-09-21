# CSS parser: at-rules no longer abort the whole stylesheet (2026-09-21)

Found via causality-browser fetching and applying a real page's CSS
(google.com): `ca_css_parse` returned NULL for the entire stylesheet the
instant it hit the first `@media`/`@font-face`/`@import`/`@keyframes`
block. Root cause was in the tokenizer, not the rule parser: `next_token`
had no case for `@` — it fell into `default: tok.type = TOK_EOF`, so the
lexer reported end-of-input at the first `@` anywhere in the source,
silently truncating everything after it, not just failing to parse that
one rule.

Virtually all real-world CSS contains at least one at-rule (responsive
breakpoints alone guarantee `@media`), so this made `ca_css_parse`
effectively unusable on any real page's stylesheet — it either worked
by accident (no at-rules present) or discarded the whole thing.

Fix: added `TOK_AT` (tokenizer now recognizes `@` instead of miscoding
it as EOF), and `skip_at_rule()` in `css.c`'s top-level `ca_css_parse`
loop — consumes tokens up to either a top-level `;` (statement-style,
`@import url(...);`) or a balanced `{...}` block (block-style, `@media
(...) { .foo {...} }`, tracking nested-brace depth via the token stream
so a rule's own braces inside the at-rule don't end the skip early).
None of these at-rules are *implemented* (no actual responsive
breakpoints, custom fonts, or animations) — they're recognized only so
they can be skipped without corrupting the rest of the parse. That's a
deliberate, scoped fix, not a partial implementation of the four at-rule
kinds.

Verified via a rule-count assertion against the internal `Ca_Stylesheet`
struct (accessible from within the codebase via `css.h`, even though
opaque to external consumers via `causality.h`): a stylesheet mixing
`@import`/`@media` (with two nested rules)/`@font-face`/`@keyframes`
with two real top-level rules produced `rule_count == 2` — exactly the
two real rules, at-rules correctly skipped as whole units, nested rules
inside `@media` correctly *not* individually promoted to top-level
rules. Also verified end-to-end through causality-browser's full
fetch→parse→apply pipeline against a local test page mixing the same
at-rules with a real `.class { color: ...; font-weight: ...; }` rule —
applied successfully where it previously logged a hard parse failure.
