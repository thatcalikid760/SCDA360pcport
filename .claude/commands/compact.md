---
description: Compress session state into STATE.md (<50 lines)
---
Rewrite `STATE.md` so a fresh session can continue with no other context. Hard limit: 49 lines.

Keep, in this order: root path + inputs; verified binary facts (title ID, entry, image base, code range, import counts); done (dated, 1 line each, merge old items); current blocker; next 3-5 steps; open questions.
Drop: narration, resolved questions, anything already in `CLAUDE.md` or derivable from `docs/`.
Never drop: addresses, error strings, file paths, unresolved risks.
After writing, confirm the line count with `wc -l STATE.md`. Output 1 line: new line count.
