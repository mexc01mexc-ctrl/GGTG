# SDD ledger — plan: docs/superpowers/plans/2026-09-29-squidward-redesign.md
Setup: Ruling: no git repo in this checkout — no worktree/commits; originals of modified files backed up to backup/ — cost if wrong: manual restore instead of git revert.
Pre-flight: T2-4 consume T1 header (declared all in T1) — consistent; T5 consumes parse API + Data::GroupBaseTitle/ScrapedGroupEntry{link} — exists; T6 consumes Session API as in T5 header — consistent.
Task 1: complete (tests: Telegram/build/ggtg/test_squidward_parse.sh → All checks passed). Task 1: Ruling: added U+0130 İ to fold table — Qt toLower keeps İ — cost if wrong: none
Task 2: complete (tests: test_squidward_parse.sh → All checks passed)
Task 3: Ruling: defined L/Capitalized/DiversityPattern directly in the anon namespace before Reader instead of declare-then-define — same behaviour, simpler — cost if wrong: none
Task 3: complete (tests: test_squidward_parse.sh → All checks passed)
Task 4: complete (tests: test_squidward_parse.sh → All checks passed)
Task 5: complete (build: cmake --build out --config Debug --target Telegram → BUILD SUCCEEDED; parse/session .o compiled)
Task 6: complete (build → BUILD SUCCEEDED; test_squidward_parse.sh → All checks passed)
Final review: opus reviewer (subagent). Important: #1 stale card, #2 card edit as page 1, #3 generation hole in settleGroup, #4 page total wrong, #5 auto mode leaks. Re-graded to Important: #8 Stop recreated per page (lost clicks), #12 closed-group dedupe key has msg id, #13 users kept as groups, #14 retry re-clicks stored position (could hit paid button), #15 stuck on bot Failed.
Final: minor (deferred): #6 base title uses bot's stylized card name instead of real peer name
Final: minor (deferred): #7 card refresh after collection wipes result view
Final: minor (deferred): #9 bot notice before card becomes final Unknown card
Final: minor (deferred): #10 Unknown card gets no collect button (spec says buttons searched as usual)
Final: minor (deferred): #11 collect button without count; saved state "Открыть базу" instead of "✓ В базе — открыть", Копировать hidden after save
Final: fixed #4 page total — TestRows fullPage check RED→GREEN, suite All checks passed
Final: fixed #12 closed dedupe key — TestRows t.me/c dedupe check RED→GREEN, suite All checks passed
Final: Ruling: #1 #2 #3 #5 #8 #13 #14 #15 fixed without a failing test first — session/UI code has no test harness outside the full app (tdesktop UI test runner needs a live account); verified by Debug build + review of the logic; live check left to Task 7 — cost if wrong: a regression shows up only in manual testing
Final: Ruling: workspace (backups of original files) kept, not deleted — the checkout has no git, the backups are the only way back — cost if wrong: a few KB of files
Task 7: pending — live check sends lookups to @skvidvarde_bot from the user's account; asked the user
Deferred minors #6,#7,#9,#10,#11: fixed on user request (real Telegram name for base title; card refresh keeps collect block; unknown reply waits 4 s for a real card; unknown reply collects via its buttons; count on collect button from "Всего N", "✓ В базе — открыть" + Копировать after save) — build8.log BUILD SUCCEEDED, build-only verification.
Task 7: live bug — placeholder 'Bычисляю' (Latin B) taken as Unknown card; live card not parsed (℮, қ, fullwidth Ｃ, Greek Ι). Fixed: IsPlaceholder by ⏳, wider fold table, late real card replaces Unknown — TestFoundation+TestLiveUserCard RED→GREEN, suite All checks passed, build9 OK
