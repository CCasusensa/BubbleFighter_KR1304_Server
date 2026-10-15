# BubbleFighter.Server agent instructions

Current submission scope (2026-10-15): the user confirmed filtering by the
original top-level directories. Keep tools/ on disk but exclude it from Git;
preserve new modules under src/ and evidence under docs/. Never force-add
tools/ without a new instruction. Its client adapters, generators and related
optional checks require the retained workspace files. Local commit correction
is authorized; no GitHub push has been authorized.

Current update (2026-10-15): read docs/battle_awards.md and the local evidence
report docs/evidence/battle_awards_validation.json first. Ordinary mode1 now
freezes durable request v3 and atomically awards W/L/D and accepted bubble/pang/
rescue counts with progress and the immutable V1 receipt. V1/V2 requests retain
no-counter semantics. Saturate at signed32 maximum, preserve all other fields;
no escape awards, new modes or full physics are implied. Same-character resets
serialize with awards; replay never restores reset counters. All22 migrations
and previous frozen fixtures remain. Debug/Release each pass52071 C++ checks,
16 frozen batches,10 runtime checks; each425 MySQL checks/30 races and513 TCP.
Existing reset suites178 MySQL/321 TCP and profile suites76 MySQL/371 TCP pass.
Three IDA functions match PE; six winner-helper cases execute408 original
instructions, zero adapters/replacements. Original UI remains paused.
The user now explicitly authorizes committing the accumulated update in this
repository's current layout. Local Git commits are allowed for this update;
pushing to GitHub remains unauthorized. Preserve ignored artifacts and private
credentials outside the commit. Earlier no-commit statements describe their
historical stages. Full server goal remains active; remaining item/activity/
platform work precedes all modes and complete battle rules.

Read docs/battle_records.md and docs/evidence/battle_records_validation.json first.
C806 cards100090/100091 now use owner-locked atomic reset/card consumption with
native S813 success0/already-zero9.43 identified fields persist in Characters;
20 result/rate inputs reset by100090,6 escape counters by100091,17 combat/rank
fields retained. These scopes and S105/S813/S802 ordering are explicit project
policy, NOT a recovered original-server algorithm. Preserve all opaque fields.
Migration22 adds version1/176-byte BattleRecords; retain all21 older migrations.
Main listeners need1..10,16..22; clan additionally11..15. Missing column rejects
before listening. Four canonical readers load counters with the current row.
New characters ignore caller counters. Current S105 and room cache refresh both
bind current records; V1 receipts remain byte-identical and never restore them.
A regression found/fixed RoomRegistry refreshing only the four previous fields.
No battle-count awards/disconnect penalties/new modes yet; new counters default0.
Do not claim full battle statistics or all modes available. Future modes must
prove their counter meanings and extend versioned scope before enabling them.
22 functions/2 chunks/87 resources match111 PE blocks;6 bounded paths execute53
original instructions,zero adapters/replacements.16 original UI files clarify
labels.43 independent field/scope cases frozen before production implementation.
Debug/Release each49428 C++(new699+62),16 frozen batches,10 runtime,zero warnings/
errors. Each178 MySQL with18 races and321 TCP validates both cards,room use,
lost response,reconnect,rollback,ownership and immutable receipt replay. Gift,
bundle,refund,social,clan,preset suites and missing-schema guards pass both configs.
845 inputs/34 changes,18 C++ format,2 Python/8 PowerShell syntax checks pass.
16 isolated database/user/credential scopes cleaned;shared MySQL and unowned
clients retained. Initial test compile/system-temp-space/cache failures retained
and fixed. Compiler temporary files use this repository's Artifacts/Temp/Compiler
with /m:1 /nr:false and CL_MPCount1; do not delete unrelated system temp files.
Two fetch resets;final GitHub compare confirms main8587859 identical0/0.
No original UI,business migration,commit or push. Full goal active: trusted
battle-counter producers required for these records,remaining item/activity/
platform systems,then all modes/full battle rules. Older entries retain history.

Read docs/settlement_profile_delivery.md and docs/evidence/settlement_profile_validation.json first.
Room S105 delivery now reloads the authenticated current CharacterRecord at all
Process/Poll queue drains; batches without S105 perform no read. Verify identity,
encode before publishing session/room caches; failed reads reject that connection.
Historical S614, reward deltas and V1 receipt bytes remain unchanged. Never replace
live room name/model from a replayed receipt. Current reads are snapshots, not a
promise that no later writer can change state. Unknown envelope fields are preserved.
This fixes current progress/wallet delivery, NOT battle-counter persistence or reset
card effects. Additional native UI evidence:74 label bindings,22 functions/2 chunks/
86 resources match110 PE blocks;6 bounded call sites execute53 original instructions,
zero adapters/replacements. Tab4 zombie,Tab6 first/rate,Tab7 rank/giveup and Tab9/10/11
fields need complete reset-scope decisions; never clear opaque groups indiscriminately.
Six independent cases frozen before code. Debug/Release each48667 C++(new616),
16 frozen batches,10 runtime checks,zero warnings/errors. Production MySQL76 checks
per invocation (two isolated invocations/config);natural four-minute two-player TCP371
per config verifies historical receipts,current S105 and restart/reconnect. Social
TCP8137/mailbox845 each,MySQL49/145/284 and all three missing-schema guards pass.
835 inputs/15 changes,six C++ format/two Python/two PowerShell syntax checks pass.
Two added literals declared;21 migrations/generated data/prior fixtures retained.
Five scopes including the first failed harness are cleaned;shared MySQL preserved.
Initial test compile and migration-filename failures retained and fixed. Project XML
line endings normalized after build with identical parsed XML; final C++ unchanged.
Two fetch connection resets;GitHub read-only compare confirms main8587859,0/0.
No client UI,business migration,commit or push. Full goal active: complete reset
scope/durable counters/atomic card consumption,remaining item/activity/platform
systems,then all modes/full battle rules. Older entries retain historical scope.


Read docs/stat_reset_protocol.md and docs/evidence/stat_reset_protocol_validation.json
first, then docs/delivery_capacity.md for the latest implemented business change.
Reset cards100090/100091 use native C806/S813 Function2 Parameter1/2. Success0,
already-zero9 and ignored22 are verified. Their effects are NOT enabled yet.
S105 FighterInfo+80/+84/+88 map to wins/losses/draws, +40 to disconnects, supported
by named native telemetry. Resource descriptions say all modes; additional Tab10/
Tab11/other counters mean four fields alone cannot prove a complete reset.
Finish per-mode mapping and durable ownership/atomic consumption; audit all S105
producers and version1 match receipt replay before activating. Never overwrite
unknown fields or infer awards/reset scope from old server structs.
20 functions/2 extra chunks/24 resources match46 PE blocks;19 bounded runs execute
518original instructions,10 construction/stream adapters,zero replacements.
Ten independent packets frozen before business implementation. Full handlers,
obfuscated send wrapper,CRT formatting and original UI not executed.
All827 preceding inputs remain unchanged;only native probe/fixed cases added.
No production C++/schema/build change or new C++/MySQL/TCP suite run in this
analysis increment. Preceding delivery-capacity report retains exact binary scope.
No new database/ticket/process scope,client UI,commit/push. SharedMySQL preserved.
Full goal active: complete remaining item/activity/platform systems,then all
modes/full battle rules. Earlier entries retain historical scopes.

Read docs/delivery_capacity.md and docs/evidence/delivery_capacity_validation.json
first, then docs/bundle_gifts.md and docs/bundle_transactions.md for ownership.
All four cash/Lucci ordinary/bundle purchase paths now check pending cash and
gifts under the recipient character lock before debit/inserts/sequence advance.
Gift admission uses the same order-independent projection. Group by item, use
the smallest saved cap, reject a pending permanent plus any other same-item part,
and bound1024 slots and signed32 bonuses. Future-dated pending gifts still reserve
capacity after clock rollback; missing bundle components fail closed. Nonlocking
pending gift reads occur only after the owner lock; terms are immutable.
These are project admission rules, not recovered original-server policy. Claims
revalidate. External writes/unrelated rewards are not universally reserved; future
reward systems must integrate capacity. No new migration or protocol message.
24 independent policy cases frozen before implementation.3 functions/2 ranges
match5PE blocks;5 bounded dispatch runs execute40original instructions, no adapters
or replacements. Full native dialogs/original UI stay paused.
Debug/Release each48051 C++(new141),16 frozen batches,runtime10;zero warnings/errors.
DeliveryMySQL137/135,each12 cross-system races;gift126+116/TCP838/832;
bundle160/34508;refund137/401;social main8137/mailbox845;clan698/3572.
Missing-schema guards pass. Clan legacy harness lacks an embedded binary hash;
parent records final hash and run ordering after hash-bound gift runs.
827inputs/22changes,15 C++ formats and2 Python syntax pass;3 added literals and
Purchase transaction change explicitly declared.21 old migrations/generated data
and frozen fixtures preserved. Failed fixture SQL and pre-clock-fix runs retained.
21 generated scopes, credentials and owned tests cleaned;sharedMySQL preserved.
No business migration,client launch,commit or push. Fetch0/0 at8587859.
Continue remaining item/activity/platform systems,then all modes/full battle rules.
Full goal remains active. Earlier entries retain their historical scopes.

Read docs/bundle_gifts.md and docs/evidence/bundle_gifts_validation.json first,
then docs/bundle_transactions.md and docs/gifts.md for inherited policies.
Native C818 now accepts391 of486 published StockSet offers;95 retain refusal.
Life components are explicitly excluded. One gift owns ordered immutable parts
and bonus; sender debit/insert and recipient all-parts/bonus/state are atomic.
Migration21 adds GiftBundles/GiftBundleComponents; preserve all20 older scripts.
Missing headers/counts/order fail closed. Both new tables are checked before
listening. Main listeners need1..10,16..21; clan additionally11..15.
Gifts create no cash slots/refundable receipts. Bonus-at-claim, recipient-level
checks and immutable delivery terms are explicit project rules. Duration begins
at claim. Claim/discard/replay exclude; lost send responses have no request nonce.
Gift admission projects all pending cash/gift parts and bonuses, checking1024
slots and signed32 balance. Purchase routes do not reserve pending gifts yet;
this is conservative admission, not universal capacity reservation.
13functions/13PE blocks;12 bounded native runs execute981original instructions,
102boundary adapters,zero replacements. Independent fixture frozen before code.
Original UI/modal/renderer stays paused; no captured original-server claim.
Debug/Release each47910 C++(new550),16 frozen batches,runtime10;zero warnings/errors.
GiftMySQL126+116 with21+12races each,TCP814/820. BundleMySQL160/TCP34508 each;
refundMySQL135/TCP401 each; social49+145+284+79/CLI6,mainTCP8137,mailbox845/843;
clanMySQL698/TCP3572 each with276 frozen frames. Missing-schema guards pass.
817inputs/26changes,12C++ formats,2Python/6PowerShell syntax pass;15 new literals
and one retired prior addition explicitly declared. Frozen data/fixtures remain.
Initial test registration,fixture SQL and refusal expectation failures retained
and corrected.16 isolated scopes and owned processes cleaned; sharedMySQL and
unowned clients preserved. No business migration,client launch,commit or push.
Fetch succeeds0/0 at8587859; full goal active. Next shared pending delivery
capacity and remaining systems,then all game modes/full battle rules. Special
activity/allowChr authority and platform SDK/C823 remain unresolved.
Earlier milestone entries retain their historical scopes.

Read docs/bundle_publication.md and docs/evidence/bundle_publication_validation.json
first, then docs/bundle_transactions.md and docs/gifts.md for transaction boundaries.
S800 publishes486 supported Kind1 bundles on native Packages800/All_Packages800,
pageType2. All7069 ordinary cards/prices and five existing categories retain their
exact bytes. Full7555cards/sixcategories/1181166bytes fit existing8192-card and
S800-only2MiB bounds; generic inbound1MiB remains. Content revision4838886565553500510
is independent of selection/clock/balance/level. Fresh connections always receive
full snapshots; same-session hits retain native40-byte empty-vector response.
Representative show/sell prices are exact resources; false option flag/type1/
duration0 retain native defaults, empty price vector follows Kind1 branch.
Card grouping is project presentation policy, not recovered server assignment.
Never publish all4110 entries or treat catalog/cache as purchase authorization.
Correct prior documentation:2000813 is supported Duration; resource-listed
exclusions are special type2 2003269 and extended Stock2004901. Count486 and
384Cash/102Lucci/12bonus are unchanged; preserve older hashed reports as history.
26functions matchPE;four native writers plus11 bounded price/filter runs execute
786original instructions/72 stream-write adapters/zero replacements.486card
expectations were frozen before implementation; no captured-server/UI claim.
Debug/Release each47360 C++,16 frozen batches,runtime10;zero build warnings/errors.
BundleMySQL160 with12 real races andTCP34508 each; both missing-schema guards pass.
Other DB suites/JS were not rerun; historical reports retain earlier binary scope.
808inputs/eightchanges,four C++ formats/two Python syntax,two added labels declared.
Preserve20migrations/generated data/prior frozen fixtures. No new transaction,
runtime patch or schema change. Two generated test scopes and owned listeners
cleaned; sharedMySQL/unowned clients preserved. Original UI remains paused.
No business migration,commit/push/client launch. Fetch0/0 at8587859; full goal active.
Next implement bundle gifts with immutable multi-component receipts and no refund,
then remaining systems and all modes/full battle rules. Special activity/allowChr
purchase authority and unified pending delivery capacity remain unresolved.
Earlier entries retain their historical scopes; this entry supersedes their
not-yet-published statement and erroneous2000813 exclusion explanation.

Read docs/bundle_transactions.md and docs/evidence/bundle_validation.json first,
then docs/stock_sets.md and docs/gifts.md for unchanged resource/gift boundaries.
Native C809/C829/C817 now route whole-bundle purchases/pickup/refunds through
authenticated owner-locked MySQL. Migration20 adds immutable CashBundles and
ordered components; preserve all19 older migrations and frozen fixtures.
CashInventorySequence allocates ordinary/bundle IDs. Cash bundles use one S810
record with stock/item=StockSet ID, quantity/days0. Claim all components+bonus
atomically; duration starts at claim. S835 precedes S810 replacement. Refund
uses saved full price and seven-day policy; claim/refund permanently exclude
each other. Lucci bundles grant directly and create no refundable receipts.
No S800 bundle publication or bundle gifting yet. Standard listed type1 policy
accepts486 (384Cash/102Lucci/12bonus); historical dates remain metadata. Do not
enable all4110 resource rows. Oversized prices, empty rewards, activity bonuses,
extended Stock IDs and unsupported component classes stay refused. allowChr
purchase authority remains unresolved; component equipment rules still apply.
Bonus-at-pickup and eligibility are project policies, not original-server facts.
Validate receipts/count/order; never reprice saved amounts/caps/bonus at pickup.
Ordinary cash purchases and gift send consider pending bundle components.
These are conservative admission checks, not universal delivery reservations.
Main DB listeners need1..10,16..20; clan also11..15. Check both new tables before
listening. Only generated isolated scopes were migrated; no business DB writes.
14functions/3resources match17PE blocks;9 bounded runs execute496original
instructions/36adapters/zero replacements. Fixed synthetic cases were created
after the initial code draft and before compilation; no captured-server claim.
Debug/Release each45886 C++(new90),16 frozen batches,runtime10;final zero warnings/
errors. BundleMySQL160 with12races andTCP332 each;refund135/137+401, gifts126
with21races andTCP494/500, social/clan MySQL/TCP and missing-schema guards pass.
806inputs/31changes,17 C++ formats,2Python/6PowerShell syntax,23 added literals
and3 explicit transaction changes. Initial fixture/trigger failures and compiler
heap failure retained; serial compilation succeeds.15 generated scopes and
owned listeners cleaned; sharedMySQL and unowned client processes preserved.
No commit/push/client launch. Fetch succeeds0/0 at8587859. Full goal remains active.
Next verify bundle card category/price consumers and publish supported offers,
then bundle gifts without refunds, other systems and all modes/full battle rules.
Original UI stays paused. Earlier milestone entries retain historical scopes.

Read docs/stock_sets.md and docs/evidence/stock_set_validation.json first,
then docs/gifts.md for the unchanged ordinary gift transaction boundary.
StockSetCatalog preserves all4110 resource rows and15374 component references.
It does NOT publish offers or enable bundle purchases, gifts, pickup or refunds.
206 rows grant bonus Lucci;133 have no items and31 have no identified reward.
23 prices are9999999999: retain uint64, never truncate or make free offers.
Four900104901..4 components resolve in XML but native classifier returns0.
97 Lucci bundles reward more than their price; activity authority is unresolved.
Do not infer authorization from full ordinary-catalog publication policy.
Keep canPresent/refundable/type metadata distinct and category tokens opaque.
34 functions/18 resources/one vtable match53PE blocks. Eight independent fixed
packets and48 bounded runs execute1456 original instructions with70 adapters.
Post-CRT decision tests do not execute numeric conversion, full loaders or UI.
Native C809 is reused; no invented bundle message. Life components deny gifting.
Debug/Release each45796 C++(new408),16 frozen batches,runtime10;zero warnings/errors.
793 inputs/14 changes,four C++ formats/two Python syntax,2999 added literals.
Preserve19 old migrations and frozen packets. No MySQL/TCP-business/JS rerun;
older acceptance reports retain their historical scopes. No DB/ticket scopes,
business writes,client patch/UI launch,commit or push. Shared MySQL and unowned
client processes preserved;owned smoke listeners exited. Fetch succeeds0/0.
Next recover bundle cash-warehouse and bonus consumers, then atomic immutable
purchase groups,whole-bundle/bonus pickup and refund exclusion. No refundable
receipts for gifts. Full11179-card publication exceeds current8192-card policy;
resolve actual representation/capacity before expanding it. Full goal active.
Earlier milestone entries below retain their historical scope.

Read docs/gifts.md and docs/evidence/gifts_validation.json first.
Ordinary gift send/list/claim/discard/owner hiding/thanks and pending notices now
use authenticated sessions and MySQL. Migration19 is additive; preserve18 older
migrations and frozen packets. Main listeners require1..10,16..19; clan also11..15.
Ordered sender/recipient locks and revalidation protect debit/delivery; claim and
drop exclude each other. Never create refundable purchases for received gifts.
Thanks is recipient-owned once after claim, sender-readable; one-side hiding
never revokes peer ownership. Periodic pending counts survive offline/restarts.
Resource canPresent permits5677/7069 items;1392 refuse. Cash process-only patch
preserves that predicate (0 permits buy/gift,1 buys only); category1300 still denies.
30days,200 visible/direction,128/24 UTF16 units,UTC dates,positive signed32 IDs and
the1/2 handled-state distinction are project policies, not recovered server defaults.
S825 extra fields also reach conditional platform callbacks; explicit empty/zero
baseline remains. StockSet/platform SDK/C823 unfinished. Original UI stays paused.
36 functions,81 resources,4 ranges match121PE blocks;79 bounded runs execute1051
original instructions and21 replacement instructions,zero adapters. Prior51 blocks,
26 runs/2959 original instructions/216 adapters reverified. No full UI execution.
Debug/Release each45388 C++(new147),16 frozen batches,runtime10,zero warnings/errors.
Gift MySQL126 each with21 races,TCP494/500;refund137/401 each. Social49+145+284+79
MySQL,8137 mainTCP/845 mailbox;clan698 MySQL,3572/3576TCP,276 frozen frames and
all missing-schema guards pass. JS639,Python21,33 signatures;13 C++ formatted,
3 Python/5 PowerShell syntax checks.784 inputs/31 changes,35 added literals declared.
Initial test default-argument compile,multi-statement SQL and legacy-fixture/empty
warehouse assertion failures retained/fixed.13 isolated scopes and owned listeners
cleaned;shared MySQL and non-owned existing client processes retained.
No business migration,original UI launch,commit or push.Three fetch network failures,
then read-only GitHub compare confirms main equals8587859,ahead/behind0/0.
Continue remaining systems,then all game modes/full battle rules.Goal remains active.
The earlier protocol-only and other entries below retain their historical scope.

Read docs/gift_protocol.md and docs/evidence/gift_protocol_validation.json first.
Twenty native gift codecs (nine requests/eleven responses and notifications),
four record structures and forty independently fixed packets are implemented.
Gift persistence, authenticated routing, payments/claim/drop/delete/thanks and
UI activation remain unfinished; do not claim the gift system usable.
IDA MCP13337 currently loads BFCLIENT02;39 functions/one range/11 resources
match51 PE blocks.26 bounded runs execute2959 original instructions with216
boundary adapters, including fixed page count4 and signed-short thanks field.
S829 first wire field is four bytes before native bool conversion; preserve bits.
S826 signed result>=0 enters success, unlike result1 in most other gift handlers.
C823 request is NOT recovered; do not invent it from S830. Unknown record wide/
short/string/detail members stay opaque until consumers establish their meaning.
1024 elements/1MiB payload are resource policies;16384 UTF16 units is native.
Debug/Release each45241 C++(gift1251),16 frozen batches,runtime10;zero warnings/
errors.768 inputs/11 changes,four C++ format/one Python syntax checks pass.
Five new literals explicitly declared;18 old migrations/frozen fixtures preserved.
No migration,DB/TCP business rerun,UI,business DB writes,commit or push this step.
No DB scopes created,owned listeners exited,shared MySQL preserved.Third fetch
succeeds0/0 after two network failures. Full goal remains active.
Next finish consumer/result evidence, canonical sender/recipient authority,
transactional delivery/claim/drop, owner-scoped hiding and thanks/notifications,
with ordered character locks, replay/race/rollback/restart verification; then
other systems and all modes. Never turn received gifts into refundable purchases.
Original UI remains paused. Older entries below retain their historical scope.


Read docs/cash_refunds.md and docs/evidence/cash_refunds_validation.json first.
C817/S824 refunds now use authenticated sessions and durable whole-purchase ownership.
Migration18 adds receipts and slot mappings; preserve all17 prior migrations.
Main DB listeners require1..10,16..18; clan listeners additionally require11..15.
Purchase, pickup and refund share owner locks and transactions. Claiming any part
makes the whole purchase non-refundable. Do not infer legacy groups from IDs;
legacy slots remain claimable without refunds. Seven days is project policy,
not a recovered original deadline. Only confirmed empty C817 text is accepted.
Refunds credit the saved full price once; repeated requests cannot re-credit.
Success sends S105,S834,S824,S810, with S824 before removal of other split parts.
S810 byte+48 is exactly1 only for eligible durable purchases. Native startup
already inserts feature1002; the existing shop patch skips9999 and retains1002.
No new client patch. Original UI remains paused and unverified.
Debug/Release each43990 C++(new service121),16 frozen batches,runtime10; refund
MySQL135/141 and TCP401 each. Eight real races have branch-dependent counts.
Social each49+145+284+79 MySQL,8137 main TCP,845 mailbox TCP,CLI6; clan each698
MySQL/3572 TCP,276 frozen frames. Main three,clan five and refund schema guards pass.
27 prior IDA/PE blocks and294 runs rerun; three additional blocks/six runs,
304 original instructions/80 adapters confirm feature selection without UI.
762 inputs/36 changes;24 C++ formats,three Python/four PowerShell syntax pass.
Explicit compatibility declarations cover Purchase/Pickup/Refund and23 literals;
original frozen method manifest,old migrations and packet fixtures are unchanged.
Preflight Statement lifetime bug2014 and initial test compile error are fixed;
failures retained. Eleven generated DB/user/private scopes and owned servers
cleaned; shared MySQL preserved. No business DB migration,commit or push;fetch0/0.
Continue gifts,thank-you messages,remaining items/other systems before all modes.
The complete goal remains active. All older entries below retain their scope.

Read docs/cash_refund_protocol.md and docs/evidence/cash_refund_protocol_validation.json first.
Native C817/S824 codecs are present but refunds are NOT routed or usable yet.
Do not enable the S810 refund flag before transactional purchase-group authority.
Current large Count purchases split into slots with the price only on the first;
never refund that slot after another part has been claimed. Do not infer old
groups from adjacent IDs. Refund duration is unknown; any limit is project policy.
C817 UI text is empty and has unknown meaning, never a trusted credential.
S824 result1 deletes the slot,4 reports expiry; S817 is another message.
The native UTF16 reader consumes embedded NUL but wcslen truncates displayed text;
strict codecs retain wire units, reject trailing/truncated/over16384 data.
27 IDA/PE blocks and294 bounded native runs,2903 instructions,117 adapters pass;
Debug/Release each43869 C++ including8644 new checks,16 frozen batches,runtime10.
No migration,DB business,TCP business suite rerun,original UI,commit or push.
Continue durable refund ownership and atomic claim/refund tests,then gifts,other
items and all modes. Full goal remains active. Older entries keep their scope.


Read docs/offline_messages.md and docs/evidence/offline_messages_validation.json first.
Durable companion chat is now connected to MySQL, authenticated C65016/S65017
and the Messenger adapter. Migration17 is additive; preserve all older scripts.
Main DB listeners require1..10,16,17; clan listeners additionally require11..15.
Use ordered character-pair locks for sends and relationship/block mutations;
commit before success, non-destructive fetch, authenticated issued-ID ack only.
Friend removal cascades queued records; either-direction block purges both ways
in the block transaction. Capacity200, retention30days, batch20 and recent200
ack/dedup IDs are project policy. Old senders stay online-only; recipients opt in.
Reconnect/restart replays durable records; same-session handoff retains in-flight
permissions. Ack means adapter handoff, not human read or proven visible display.
Two DWORDs preserve64-bit JS IDs. Process crashes and manual resend after a lost
send receipt can duplicate display/messages; never promise exactly-once.
Debug/Release each35225 C++(new101),284 mailbox MySQL,845 mailbox TCP and8137
three-player TCP; groups145,social49,prior79,CLI6,runtime10. Clan each698 MySQL/
3572 TCP with276 frozen frames and five schema guards; main has three guards.
JS638,Python21,33 preflight signatures;26 IDA/PE blocks and five bounded native
runs,72 original instructions,zero adapters. Original UI remains paused.
742 inputs,24 changes;12 C++ formatted,one Python/three PowerShell syntax checked.
19 added literals/SQL declared;16 old migrations and all prior fixtures unchanged.
Nine isolated schemas/users/private scopes and owned servers cleaned; shared
MySQL preserved. Two fetch network failures followed by successful0/0 retry.
No business database changes,commit/push. Other item,gift/refund/thank-you systems
come before all game modes and complete battle rules. Full goal stays active.
The protocol-only and older entries below retain their historical scope.

Read docs/offline_messages_protocol.md and docs/evidence/offline_messages_protocol_validation.json first.
Current SDK Note events0x1008/0x1016 dispatch to game callback slots+28h/+50h;
both are44E030 no-ops. Handled flag+16 does not prove visible delivery or human
read. Gift thank-you messages are a separate subsystem. SDK IDs are not TCP IDs.
15 functions,7 ranges,4 resources match26 PE blocks. Five bounded original runs
execute72 instructions with zero adapters, without platform envelopes or UI.
Two companion codecs65016/65017 exist but are not routed, persisted or activated
in the adapter. Offline messages are NOT usable yet; existing online chat stays.
Eight independently fixed fixtures; strict64-bit IDs,20-record batches,32-name/
256-text UTF16 units, timestamp1..4102444800 are project policy. Acknowledge means
adapter handoff only, not human read. JavaScript must preserve all64 ID bits.
Debug/Release each35124 C++(new379),16 frozen batches,10 runtime checks; zero
warnings/errors.735 inputs,11 changes; old16 migrations/fixtures unchanged.
Next implement transactional owner-scoped durable queues, block/friend removal
rules, bounds/expiry, explicit opt-in, idempotent acknowledgements, reconnect/
restart replay and adapter deduplication. Validate isolated DB/TCP and offline UI
models; original UI remains paused. Do not promise exactly-once visible display.
This protocol increment has no new migration, DB/TCP business suite or adapter
activation. No business DB writes,commit/push;fetch0/0. Complete other systems
before all game modes. Full goal active; older entries retain historical scope.


Read docs/friend_groups.md and docs/evidence/friend_groups_validation.json first.
Friend groups now have owner-scoped MySQL persistence, authenticated C65012
routing, explicit S65013 subscriptions, conditional moves and empty deletion.
Migration16 is additive; main DB listeners require migrations1..10 plus16,
and clan functionality also requires11..15. Do not migrate business data.
Ordered character-pair locks protect assignments against friend removal; database
foreign keys remove both owners' assignments when the friendship is deleted.
Only the requesting owner sees personal groups. Roster/group snapshots share one
outgoing batch; the adapter stages the roster until its group snapshot arrives.
SDK0x4008 stays delete-friend; target0 ungroup is project companion capability.
Native104-byte groups use F96740 copy construction and nested friend vector+92;
opaque20/88/89 retain empty-template zero values. New runtime signature verified
through IDA MCP13337 and the original PE. SDK IDs are not main TCP message IDs.
Debug/Release each34745 C++,8137 social TCP,145 group MySQL plus49+79 prior
checks; JS496,Python21,33 full-preflight signatures and42 bounded native runs.
Each clan suite passes698 MySQL,3572 dual-port TCP and five schema guards.
All five isolated scopes are cleaned; final source/binary hashes are in validation.
Original UI stays paused. Offline models and synthetic TCP do not prove live UI.
32groups/200assignments,strict names,expected source and quota are project policy.
No commit/push; latest fetch0/0. Continue offline messages, remaining items,
gifts/refunds and other systems before all game modes. Full goal active.
The protocol-only and older entries below retain their historical scopes.


Read docs/friend_groups_protocol.md and docs/evidence/friend_groups_protocol_validation.json first.
Two strict project companion codecs C65012/S65013 exist; they are not routed or
persisted yet. Do not claim friend groups usable. Existing Messenger adapter is
unchanged and still uses a flat roster. Native SDK IDs 0x4007/8/9 and0x4101/2/3
are not main TCP message IDs. 0x4008 is used by the current UI delete-friend
confirmation; do not reinterpret it as ungrouping from its class name alone.
Native names have32 UTF16 units including NUL, copied at most31; SDK length uses
compact integers. Nested CNMCateFriendInfo stride104/vtable1631944 has friend
vector+92; base category stride92/vtable1631968 cannot own that nested vector.
31 functions/11 resources/10 ranges match52 PE blocks.42 bounded runs,5647
original instructions/108 buffer-memcpy adapters; no full SDK envelope or UI.
Debug/Release each34706 C++(new596),16 frozen batches,10 runtime smoke;
zero warnings/errors.12 independent companion fixtures,15 migrations unchanged.
32 groups/200 assignments,one personal group per friend,strict names and
expected_group_id are project policy. Next implement private owner-scoped MySQL
groups/conditional moves/empty deletion and friend removal cleanup, subscribe
only explicit companion clients, adapt nested native model and SDK requests;
validate isolated DB/TCP and offline adapter. Original UI stays paused.
No MySQL/business DB writes,commit or push this increment;fetch0/0. Continue
other systems before all game modes. Full goal active. Older scope is historical.


Read docs/clan_plaza.md and docs/evidence/clan_plaza_validation.json first.
Main C2028 now authorizes canonical whole-party membership and enters isolated
S117 type6 clan plazas, including solo and waiting-room sources. No new codec
or migration; clan/bridge codecs53, migrations15. This is independent of80.
Names come from one repeatable-read snapshot, never the untrusted request.
All members accepted100..102 in the exact target clan; training99 only100 and
not in80; leader-only,maxparty8,capacity16,10-second residency are project policy.
Lock order coordinator/party -> clan operations -> completed DB read -> source
room -> target plaza. Hold operations through final scene/party commit. Public
invitation and approved visit check the complete candidate group; no mercenary
exemption in private plazas. Registry token/clan capture is observation, not consent.
Check actual committed scene before per-connection adoption; membership loss
permanently revokes the current whole-party sessions and pending reservations.
C105 returns menu, C106 ordinary plaza; same-category handoff retains private
residency, cross-category commits ordinary destination. Main scene6 grants no
ordinary-plaza officer authority. C2029 is a separate request, not a reply.
Keep wide_at80 unknown and UINT64_MAX: native113B7B0 storesFFFFFFFF at+80/+84.
Initial draft zero was corrected against two new PE-equal constructor functions.
Prior30 blocks/52 bounded runs rerun:2029 original instructions/75 adapters.
Debug/Release each34110 C++ (new396),16 frozen batches,698 clan MySQL(new36),
clanTCP3557/3568 (14 private actions,10 channel,34 clan assertions,276 frames),
five schema guards. Full socialTCP7447/seven games/78 handoffs;MySQL49+79,
accountCLI6/channelCLI16/runtime10,381 JS/21 Python; no original UI.
Zero final build warnings/errors;26 C++ format,2 Python/1 PowerShell syntax;
718inputs/34changes. Two production gaps caught by tests fixed: SceneRoster
rejected type6 and solo C106 refreshed instead of returning to ordinary plaza.
Four isolated scopes cleaned; shared MySQL and original samples preserved.
Fetch0/0; no UI,business migration,commit or push. Full goal remains active.
Continue other systems (friend groups, offline messages, other items, gifting/
refunds) before all modes. Clan battle matchmaking/gameplay and stage15 real UI
remain unfinished/unverified. Older entries preserve their historical scope.


Read docs/clan_plaza_protocol.md and docs/evidence/clan_plaza_protocol_validation.json first.
Main C2028 codec is present (53 clan/bridge codecs total), but private plaza
admission and live type6 scenes remain unimplemented. Do not claim them usable.
Native request: signed cached DWORD sign-extended to64, counted UTF16 name.
Both are untrusted. Gate permits cached training99 and checks only party-row
equality; it cannot authorize identity, roles or whole-party server admission.
S117 type6 uses text_at88/context+356 and stage15/Scenes/stage_Clan/track.x;
type3 uses text_at8/context+332 and stage3. wide_at80 meaning remains unknown.
C2029 is a separate client writer, not an inferred C2028 reply. C105/C106 common
exit paths are static evidence; complete stage15 UI is not verified.
19 PE-equal functions/chunks,eight resources,three tables;17 fixed fixtures,
52 bounded runs/2029 original instructions/75 I/O-string adapters; no UI.
Debug/Release each33714 C++ (new2088),16 frozen batches,10 runtime checks;
zero warnings/errors,4 C++ format/1 Python syntax,710inputs/11changes.
15 migrations/old fixtures unchanged. MySQL/full multiplayer TCP not rerun in
this codec-only increment; prior channel milestone remains historical evidence.
Native-probe missing import and UTF8 declaration-tool setup failures retained/fixed.
Continue actual trusted private clan plaza and complete-party movement/access/
residency before remaining systems/all modes. No UI,business migration,commit,
push or shared MySQL shutdown. Full goal active.


Read docs/clan_channel.md and docs/evidence/clan_channel_validation.json first.
Explicit category80 now supports trusted solo/whole-party channel handoff.
Require clan service and all three80 rule keys; MaximumMercenaries0..3.
Fresh consistent DB identity/enabled/level/clan reads gate preparation and commit.
Accepted ordinary leader100/101/102, training99 cannot anchor, outsiders count
against configured cap. These are project policy, not complete native server rules.
Release DB transaction before scene locks. Order: coordinator/party -> clan
operations -> completed DB read -> source room -> destination plaza.
Publish all committed categories under operations before per-session adoption;
early main binding when80configured preserves later C25/auxiliary authority.
C1036/manual C1108 are refused in80; canonical S1016, no member mutation.
Public party joins check the full target group. Residency checks party shape,
ordered identities, business revision and10-second monotonic fallback. Mark all
current members revoked atomically and close their main sessions; old reservations
also reject revoked members. No guessed forced-channel protocol. S201 is display.
14 PE-equal functions/chunks,nine resources,one jump table;5 fixed packets,
4 bounded S8 dispatch runs/44 original instructions/2 feature304 adapters.
Debug/Release each31626 C++ (new262),16 frozen batches,662 clan MySQL (new30),
3200 clan TCP/10 channel actions/50 S201/33 clan assertions/276 frozen frames,
five schema guards. Full social TCP7447/seven games/78 handoffs;social49+prior79,
accountCLI6/channelCLI16/runtime10. Adapter381 JS/21 Python, no original UI.
Zero final warnings/errors;28 C++ format,4 Python/1 PowerShell syntax;704inputs/
42changes. Two TCP fixture/order failures and one in-use EXE linker failure
retained and resolved; final binaries reran both full isolated suites.
Twelve generated scopes and owned servers cleaned;shared MySQL/client preserved.
Fetch0/0;no new codec,migration,business write,UI,commit,push. Goal active.
Category80 uses ordinary channel plaza/type3. Separate C2028/type6/stage15 and
clan-battle matchmaking/gameplay remain unfinished, followed by other systems
and all modes. Older entries below preserve historical scopes and counts.

Read docs/party_clan_identity.md and docs/evidence/party_clan_identity_validation.json first.
S201 native member+44 (values_at32[3]) now carries trusted canonical clanID.
First main login/reconnect loads DB before S201 without early C25 or auxiliary
authentication. Accepted100/101/102 use ID; pending/absent/unavailable use0.
Business-handler revision plus10-second monotonic refresh updates main/whole
party even after auxiliary loss. Changed ID retains equipment/opaque defaults,
broadcasts a complete roster, invalidates stale invitations and scene reservations.
Display cache never authorizes channel/clan operations; reports cannot write it.
14 PE-equal functions/chunks,ten resources;12 fixed samples/12 counter runs/
366 original instructions/zero adapters. Native S201 reader/UI not executed.
Correction to older plans: private clan plaza C2028 and category80 C53 are
distinct entry paths. S117 type6/stage15 is not proven required for category80.
8596E0 proves feature304 threshold<=3 only for its readiness/start predicate;
channel admission remains separate. Neither category80 nor C2028 is enabled.
Debug/Release each31364 C++ (new139),16 frozen batches,632 clan MySQL,2507 clan
TCP (38 full S201/33 clan assertions/276 frames),five schema guards. Full social
TCP7447/seven rounds/78 handoffs;social49+prior79,accountCLI6/channelCLI14/runtime10.
Zero warnings/errors;12 C++ format,6 Python/1 PowerShell syntax;694inputs/23changes.
Two initial TCP failures were test heartbeat service during a longer DB-refresh
wait and omitted pre-approval pending S1016. Corrected exact checks; both final
suites passed, production ordering/frozen fixtures unchanged; failures retained.
Six isolated scopes/owned servers cleaned;shared MySQL/client inputs preserved.
Fetch retried after reset,0/0;no new codec,migration,business write,UI,commit,push.
Next implement trusted whole-group category80 admission/residency revalidation
and the separate C2028 private clan plaza, then other systems and all modes.
Full goal remains active. Older entries retain historical scopes and counts.

Read docs/clan_presence.md and docs/evidence/clan_presence_validation.json first.
C1045 (two u32 reported level/scene) is now strictly accepted on a current clan
binding, without changing any authority or sending an invented S1046. Total52
clan/bridge codecs. S1012 +36 gets one main-owned scene snapshot after the existing
DB membership read. +28/S1014/S1070 opaque fields stay unchanged. Menu1, room or
practice2 (including waiting), plaza3; transitioning/absent0 means unknown, not
proven offline. Auxiliary reconnect preserves main presence; main revocation and
handoff preparation hide it, commit/cancel restore it. Scene/category update
atomically, and category80 is excluded from ordinary plaza officer authority.
18 PE-equal functions/chunks,ten resources;8 fixed fixtures/9 bounded writer runs/
393874 original instructions/zero adapters. No complete native UI/socket run.
Debug/Release each31225 C++ (new153),16 frozen batches,632 clan MySQL,1791 clan
TCP (15 new presence queries/276 frames),five schema guards. Final three-player
TCP7447/seven rounds/78 handoffs;social49+prior79,accountCLI6/channelCLI14/runtime10.
Zero build warnings/errors;ten C++ format/six Python syntax;689inputs/22changes.
Initial Debug social TCP exposed a test-only global ordering assumption between
independent plaza and party queues. Require each of (511,503,501)/(201,206) in
order, preserve exact roster/equipment checks, reject extras; both full suites
rerun. Failed report retained, production order and frozen fixtures unchanged.
Six isolated scopes/owned servers cleaned;shared MySQL/client inputs preserved.
Git fetch0/0;no new migration,business write,originalUI,commit or push.
Category80 remains disabled. Original error4 supports mercenary/mixed-clan
parties;feature304 cap unknown. S117 type6 selects stage15, unlike plaza type3;
do not claim clan plaza by only changing its category. Next implement native
clan scene dependencies and trusted complete-group admission/revalidation,
then remaining systems and all modes. Full goal remains active. Older entries
retain their historical validation scopes.

Read docs/clan_training.md and docs/evidence/clan_training_validation.json first.
Training99 join/manual exit/graduation now use trusted main/clan sessions and MySQL.
Five new codecs C1106/S1107/C1108 and main C948/S1515 bring the total to51.
S1107 signed first DWORD is either failure<=0 or clanID>0, not a success prefix.
C948/C2025 cannot grant membership/appearance. Shared member PK excludes dual
ordinary/training membership;99 has only role100 and no officers. Migration15
adds durable history; previous14 unchanged, business DB untouched. Level1..45
join, persistent level>45 graduation,60-second rejoin cooldown and ten-second
main polling are explicit project policy. -60 reports cannot grant graduation.
Ordinary join/create clears old exit status but preserves graduation. Reconnect
gets durable -20/-60 then canonical0, once per change per clan connection.
18 PE-equal functions/chunks,13 resources;13 fixed payloads/13 runs/861 original
instructions/20 adapters.12 native fixtures;unknown exit value codec-only.
Debug/Release each31072 C++ (training169,new main12),16 frozen batches,632 clan
MySQL (training84,creation+2);TCP1604 with16 training/35 transfer/8 disband actions,
2 maintenance runs/276 frames,five schema guards. Full three-player TCP7447,
seven rounds/78 handoffs;social49+prior79,account CLI6/channel CLI14/runtime10.
Zero final warnings/errors;21 C++ format,3 Python/2 PowerShell syntax;682 inputs/
33 changes. Six isolated scopes/owned servers cleaned;original samples and shared
MySQL baseline processes preserved. TcpPacket test equality build failure and
EOF heartbeat race retained/fixed; no frozen expectation changes. Git fetch0/0.
No original UI,business migration,commit or push. Feature202 UI visibility still
unverified;clan-channel80,other systems and all modes remain unfinished. Goal active.
Older entries below retain their historical validation scopes.

Read docs/clan_disband.md and docs/evidence/clan_disband_validation.json first.
C1029/S1030/S1031 now implement trusted ordinary schedule/cancel; migration14
adds durable maintenance removal notices. Old 13 scripts must remain unchanged.
Use explicit future --clan-maintenance-unix; no inferred original calendar.
--clan-maintenance is an offline command with no listener. It and clan service
hold one database-scoped named lock. Due clans/members and notices commit as one
batch (limit10000); receipts/accounts/characters/inventory survive. No business
migration performed. Main plaza epoch and master101 authority are rechecked.
Native -40 clears cached clan ID; canonical no-clan profile follows once per clan
connection. Reconnect repeats durable notice; joining/creating clears it atomically.
16 PE-equal functions/chunks,9 resources;13 fixed payloads,13 runs,883 original
instructions,3 memory adapters. No complete native UI; wrong IDA default rejected.
Debug/Release each30891 C++ (new160),16 frozen batches; disband MySQL91+prior455,
clan TCP1492/1483 (heartbeat timing),8 disband actions,2 maintenance runs,276
frozen frames,four schema guards; full three-player TCP7447/seven rounds/78 handoffs,
social MySQL49+prior79,account CLI6/channel CLI14/runtime10. Zero warnings/errors;
15 C++ format,3 Python/2 PowerShell syntax;671 inputs/27 changes. Five isolated
scopes/owned servers cleaned; shared MySQL/samples preserved. Initial C++ default
argument/shadowing and cancellation-test ID errors retained/fixed.
Git fetch still0/0; no UI,business migration,commit or push. Full goal active.
Next training99 boundaries,clan-channel80 authority,other systems,then all modes.
Older entries below retain historical scopes and validation counts.


Read docs/clan_master_transfer.md and docs/evidence/clan_master_transfer_validation.json first.
Native C1057 offer / C1060 target answer / C1054 requester commit now implement
ordinary master transfer; only committed S1056 changes roles. Both trusted main
sessions must retain available plaza epochs and clan connection generations.
Binding copies/Find only observe inboxes; consuming socket owners alone keep them
alive. Reauthentication, disconnect, scene changes,60-second expiry or consumed
grants cannot revive old consent. These limits and correlation IDs are project policy.
MySQL locks sorted characters/accounts then clan, atomically demotes101 to100 and
promotes100/102 to101; precommit authorization and second-write rollback verified.
No C1039 shortcut, new migration, training99 or disband/channel80 implementation.
14 functions/14 chunks,one range,eight resources match current13337 BFCLIENT02/PE;
wrong default13341 sample was rejected.24 fixed fixtures,25 bounded runs,4587
original instructions,48 adapters; no complete UI execution. Eight codecs added.
Debug/Release each30731 C++ (codec891/consent216),16 frozen batches, transfer
MySQL61+prior394,clan TCP1287/35 transfer actions/276 frames,three schema guards;
three-person TCP7447/seven game rounds/78 handoffs/two group transfers,social
MySQL49+prior79,account CLI6/channel CLI14/runtime10. Zero build warnings/errors;
21 C++ format,three Python/one PowerShell syntax,660 inputs/31 changes.
Seven isolated scopes/owned servers cleaned; shared MySQL and samples preserved.
Initial test-field/menu-S170/idle-heartbeat expectation failures retained/fixed.
No original UI, business migration, commit or push. Full goal remains active.
Next disband/training boundaries,clan-channel authority,other systems then all modes.
The entries below retain their historical scopes and validation counts.


Read docs/clan_membership_business.md and docs/evidence/clan_membership_business_validation.json first.
Ordinary clan recruitment, join/approve/reject/cancel/leave/kick/deputy changes now
use trusted sorted character locks, per-clan lock and commit-time authorization.
Migration13 expands recruitment to 0/1/2; never edit the preceding twelve scripts.
Native main C2025 is decoded but cannot grant any membership. S1034 result2 is
TRAINING-specific; ordinary autojoin's result1/status100 then S1016 is project
ordering, with original UI still unverified. Approval seeds actual pending profile
before S1041 because 51D330 reads cached pending ID. Master consent is not bypassed.
Notifications are ordered per process/current connection generation, bounded to
64 batches. Overflow closes/reconnects; offline recovery is a profile reload.
Global clan operation serialization limits throughput; no durable/cross-process
event delivery is claimed. Unknown roster fields remain zero/unknown baselines.
28 PE-equal functions/chunks, one range, nine resources; four fixed fixtures,
eight write-body runs/304 original instructions/32 adapters, no full UI/socket.
Debug/Release each: 29,624 C++ (mutation172/auth169), 16 frozen batches; mutation
MySQL95 plus profile40/create92/directory59/roster108; clan TCP913 (31 mutations,
3 main reports, 276 frames), three schema guards; full three-player TCP7447 with
seven ready/load/start/movement/return rounds, 78 handoffs including 13 cross-category,
two collective transfers; social MySQL49+prior79, account CLI6/channel CLI14,
10 runtime checks. Zero final warnings/errors, 17 C++ format/three Python syntax,
648 inputs/30 changes. Four isolated scopes and owned servers cleaned; shared
MySQL/samples preserved. Initial missing-test-header build failure retained/fixed.
No business migration, original UI, commit or push. Full goal remains active.
Next master-transfer consent, disband/training boundaries, clan-channel authority,
remaining systems and all modes. The entries below preserve historical scopes.


Read docs/clan_roster_queries.md and docs/evidence/clan_roster_query_validation.json first.
Trusted C1011/C1013/C1069 roster reads now connect to authenticated sessions and MySQL.
Accepted roles 100/101/102 read members; only 101/102 enumerate pending applicants.
Explicit repeatable-read snapshot includes permissions, profile and roster; fresh
S1018 precedes S1070. Native cached recruitment 0 emits per-applicant C1039 rejections.
C1039 and C1102 business remain unsupported: do not claim approval/stop flow complete.
Unknown stats/date fields use zero baselines; native scene 0 means unknown, not offline.
20 PE-equal functions/chunks, nine resources; unchanged 27 codec probes rerun with
7,802 original instructions/61 adapters/15 rows, containers and UI skipped.
Debug/Release each pass 29,441 C++ (new session 85), 16 frozen batches, roster MySQL
108 + profile 40 + creation 92 + directory 59, clan TCP 663 (13 roster/25 directory
queries, 276 frames), two missing-schema guards and 10 runtime checks. Zero build
warnings/errors; ten C++ format/two Python syntax checks. 637 inputs/18 changes.
Both isolated scopes and own servers cleaned; shared MySQL and samples preserved.
No migration/business write/UI/commit/push. Recruitment remains 0/1; add migration
for 2 together with trusted recruitment/member transactions and notifications.
Full three-player gameplay results remain historical; entire goal remains active.
Next finish native stop/reject, join/approve/cancel/leave authority and broadcasts,
then clan-channel authority, remaining systems and all modes.
The entries below preserve historical scopes and validation results.


Read docs/clan_directory.md and docs/evidence/clan_directory_validation.json first.
C1019/S1020 codecs and authenticated read-only MySQL directory are connected.
Five rows, native modes 4..8 and default 6/page 1 are proven. Exact-name matching,
bounded pages and ordering are project policy; unknown fields and unavailable
last-login DATE use explicit zero/empty baselines. Name-adjacent DWORD is opaque
row +88, not recruitment (+72) or icon (+84).
19 PE-equal functions/chunks, 18 resources; 14 fixed fixtures, 14 bounded runs,
15,129 original instructions, 195 adapters, 16 rows; date/containers/UI bypassed.
Debug/Release each: 29,356 C++ (directory 3,800), 16 frozen batches, directory
MySQL 59 + profile 40 + creation 92, clan TCP 546 with 25 directory queries/276
frames, two missing-schema guards, 10 runtime checks, zero warnings/errors.
11 changed C++ format/three Python syntax checks pass; two isolated scopes cleaned.
No new migration. Recruitment still 0/1 in DB; mode 2 needs a NEW migration.
Next connect roster/applicant reads, recruitment authority, membership transactions
and notifications. Category 80/other systems/all modes unfinished. Original UI
paused; full three-player/social suite results remain historical. Shared MySQL
and samples preserved; no business migration, commit or push. Full goal active.
The following entries retain their historical validation scope and limits.


This checkout is the active development tree requested by the user on 2026-10-14.
Upstream: https://github.com/K1roSama/BubbleFighter_KR1304_Server
Starting commit: 85878590313d9895d29d452647ba6ae93d25089e.
Work remains on branch development/system-completion; do not push.

Read docs/clan_roster.md and docs/evidence/clan_roster_validation.json first.
Eight roster/applicant/recruitment codecs added; 32 clan/bridge codecs total.
Native controls confirm recruitment 0 stop, 1 manual, 2 automatic. S1103 skips
its first DWORD and updates mode only on nonzero second DWORD. Stop queries
applicants, whose native consumer sends individual rejection requests.
Roster counts are checked against remaining bytes/minimum row before allocation.
Eighteen PE-equal functions/chunks and three resources; 27 independent fixtures,
27 bounded runs/7,802 original instructions/61 adapters, 15 parsed rows. Native
row-container construction/destruction is skipped after reads, UI calls observed.
Debug/Release each pass 25,556 C++ (2,358 added), 16 frozen batches, 10 runtime
checks; zero build warnings/errors. Four C++ format/Python syntax/frozen checks
pass; 622 inputs/10 changes, one new error literal. No new session or persistence
business. Existing IsRecruiting 0/1 must be extended by a NEW migration for mode 2,
not by changing frozen migration eleven. Next search/field evidence, recruitment
transactions, membership authority and notifications. No TCP/MySQL suites rerun,
original UI, business migration, shared MySQL shutdown, commit or push. Goal active.
The following entries preserve their previous scope and historical validation.

Read docs/clan_membership.md and docs/evidence/clan_membership_validation.json first.
Twelve strict membership codecs are added; they are NOT connected to sessions,
member transactions or notifications. S1037 has old/new, S1041 has new/old status.
S1034 writes status before checking its result; preserve trusted state on failure.
C1039 does not establish master-transfer consent or permission. Twenty-two PE-equal
functions/chunks, one raw range, 23 resources, 32 pre-implementation fixtures,
38 bounded runs/10,451 original instructions/124 CRT or string-assignment adapters.
Debug/Release each pass 23,198 C++ (1,676 added), 16 frozen batches and 10 runtime
checks; zero warnings/errors. Four C++ format checks/Python syntax and frozen
manifests pass; 617 inputs/9 changes. Six additional roster functions are a static
investigation only. A nonrecruiting S1070 consumer rejects applicants individually.
Next finish roster/recruitment evidence, then authenticated member transactions,
notifications and clan-channel authority. No TCP/MySQL suite rerun for this isolated
codec addition; older integration counts remain historical. Original UI paused,
samples/shared MySQL preserved; no business migration, commit or push. Full goal active.
The following creation and older entries retain their original scope and results.

Read docs/clan_creation.md and docs/evidence/clan_creation_validation.json first.
Native S1028 success prepares main C806(item_id, 1, empty); its second DWORD
is the item ID, while the third remains opaque with project zero baseline.
C1027 atomically creates the clan/master, debits one owned 100819 card and
persists a receipt. C806 only confirms a committed receipt and sends current
S813/S802; replays and same-session handoff never debit again. Migration twelve
is additive; enabling the clan listener requires migrations eleven and twelve.
Creation validates catalog, owner, membership, uniqueness and authorization at
commit decision; SQL failure/revocation rolls back all writes. Category 80 and
membership management remain unfinished. 15 PE-equal functions/17 chunks,
nine pre-implementation fixtures, 14 bounded runs/1,953 original instructions
and nine packet-write adapters. No full native handlers, sockets or UI executed.
Debug/Release each pass 21,522 C++ (creation 79, auth 158), 16 frozen batches,
388 clan TCP/276 fixtures, 92 creation + 40 profile MySQL, prior 7,447 TCP,
seven three-player game rounds, 78 handoffs (13 cross-category), two party moves,
128 prior MySQL, 6 account CLI, 14 channel CLI, 3 missing-schema guards and
10 runtime checks. Final zero build warnings/errors; 16 changed C++ format
checks, Python syntax, 612 input hashes/27 changes. Five isolated scopes cleaned.
Shared MySQL, samples and frozen expectations preserved; no business migration,
UI, commit or push. Initial probe/build/test setup failures are retained and fixed.
Next implement authenticated clan membership management/channel authority,
then remaining systems and all modes. Full goal active; older entries are history.

Read docs/clan_authentication.md and docs/evidence/clan_authentication_validation.json first.
C25/S21 now issue 30-second one-use credentials bound to a trusted main account/
character. Optional --clan-port enables the independent original clan listener,
C1005 authentication and fresh MySQL S1016/S1018 profile reads. Main handoff keeps
this same session; main teardown/rejection and new clan generations revoke old
bindings. Migration eleven is additive; runtime manifests are Version 2 with a
separate ClanPort. C1027 explicitly returns generic failure until atomic owned
item consumption/creation is implemented. Clan category 80 remains disabled.
13 IDA/PE functions and seven resources match; no new native instruction runs.
Debug/Release each pass 21,431 C++ (new auth 146), 16 frozen batches, 307 clan TCP,
276 existing wire fixtures, 40 clan MySQL, 7,447 prior three-player TCP, social
MySQL 49 + preceding 79, account CLI 6, channel CLI 14, two missing-schema guards
and 10 ordinary runtime checks. Zero build warnings/errors; 16 changed C++
format checks, Python syntax, 331 JS, 21 Python and 32 signatures pass; 603 inputs.
Four failed attempts were fixed and preserved, including MySQL 2014 from an
unreleased schema result. Regression whitespace normalization is token-equivalent.
Eight isolated databases/users/credentials and owned servers cleaned; shared
MySQL and original samples preserved. No business migration, original UI, commit
or push. Next resolve S1028 inventory side effects and implement transactional
creation/management, then clan-channel authority, other systems and all modes.
The full goal remains active. Entries below describe historical milestones.

Read docs/clan_profiles.md and docs/evidence/clan_profile_validation.json first.
S1016/C1017/S1018 codecs now complete the profile query wire path (12 clan/bridge
codecs in total). Roles 1/100/101/102 and the native officer UI predicate are
proven client facts, not trusted server authority. S1016 compares the date to
2100-01-01, not the current time. Preserve opaque DATE bits/unknown scalars.
C1027 item_id/item_parameter come from the selected Function=22 catalog item;
ownership and transactional consumption remain to be implemented. Do not infer
all S1028 success inventory effects from its obfuscated consumer.
26 IDA/PE functions, three ranges and 17 resources match. 21 independent fixed
fixtures, 41 bounded native runs, 532,882 original instructions, 109 CRT/string
assignment adapters. Debug/Release each pass 21,285 C++ (profile 1,111; prior
transport 6,212), 16 frozen batches and 10 ordinary runtime checks, zero build
warnings/errors. Four C++ format checks and Python syntax pass; 588 inputs.
Two probe setup failures were fixed and retained without changing fixtures.
Next implement one-use C25 credentials tied to trusted game sessions, separate
clan authentication/listener and MySQL profile reads; then atomic creation once
inventory side effects are resolved. No listener, auth, clan persistence/category
80 or new clan TCP/MySQL acceptance yet. UI remains paused. Shared MySQL and
original inputs are preserved; no business migration, commit or push. Full goal
active. Older entries below keep their historical scopes and counts.

Read docs/clan_transport.md and docs/evidence/clan_transport_validation.json first.
The native clan service uses a separate four-byte frame, per-frame XOR and
UTF-16Z strings. Nine codecs now cover main C25/S21 and clan S1002/C1003/
C1005/S1006/C1015/C1027/S1028. They are not wired to a listener or session.
Credentials and unknown scalars do not authorize identity, clan membership,
item consumption or category 80. Next resolve S1016 profile/member authority
and create-item parameters, then implement authenticated binding/persistence.
20 IDA/PE functions, four ranges and 16 resources match. 276 independent
fixtures and 552 bounded native runs execute 1,251,933 original instructions
with 12 CRT adapters. Full handlers, sockets and original UI are not executed.
Debug/Release each pass 20,174 C++ (clan 6,212), 16 frozen batches and 10
ordinary runtime checks, final builds with zero warnings/errors. Six C++
format checks and Python syntax pass; 585 inputs recorded. Two initial C4244
test warnings were fixed and retained. No new clan TCP/MySQL acceptance or
migration. Prior party-channel TCP counts below are historical, not rerun for
this codec-only increment. No original UI, shared MySQL shutdown, commit or
push; the full goal stays active. Older entries preserve historical scope.

Read docs/party_channel_transfers.md and docs/evidence/party_channel_validation.json first.
Whole-party cross-category handoff is now implemented for configured destinations.
Only the authenticated leader initiates C53; all members must advertise companion
capability and pass trusted eligibility. Each connection prepares and proves itself.
All C54 entries precede any S9, and all C55 acknowledgements precede collective
plaza admission/source-room departure. These barriers and deadlines are project
policy, not proven original server timing. Before any S9, failure restores sources;
after a switch and before commit, failure closes the group. After common commit,
disconnection affects only that member; queued target departures remain visible.
The adapter declares capability on initial S201; original UI remains paused.
Debug/Release each pass 13,962 C++ (party channel 1,309), 16 frozen batches,
7,447 TCP, 78 handoffs (13 cross-category), seven three-person games and seven
queued-movement rounds. Two party migrations continue into new shared games.
MySQL suites, 14 channel CLI and 10 ordinary runtime checks pass; zero build
warnings/errors. Eight IDA/PE functions, eight native leader cases (69 instructions,
zero adapters), 331 JS/18 Python/32 signatures pass. 578 inputs verified; three
isolated runs cleaned. Test expectation failures are preserved; no wire or SQL
migration added. Clan authority/category 80, other systems and all modes remain
unfinished. No original UI, business migration, shared MySQL shutdown or push.
The older entries below preserve each increment's historical limits and counts.

Read docs/channel_worlds.md and docs/evidence/channel_worlds_validation.json first.
Configured categories 20/60/70 now have isolated rooms/plazas/invitations/transfers.
Login starts in free category 60; optional level ranges are explicit project policy,
not original thresholds. Authenticated SOLO cross-category moves enter the target
plaza, retaining source leases until commit. Multi-member parties are refused intact;
collective channel migration and clan category 80 remain unfinished. Same-category
handoff preserves the current scene and pending queues. All worlds share settlement
recovery while social identity stays global. No new native codec or SQL migration.
Debug/Release each pass 12,653 C++ (worlds 103), 16 frozen batches, 6,006 TCP,
54 handoffs including seven cross-category, five three-person games in category 20,
MySQL suites, 14 channel CLI and 10 ordinary runtime checks. Zero build warnings/errors.
Six IDA/PE functions, four resources and one jump table match; six bounded original
mapping runs execute 25 instructions with no adapters. 307 JS/18 Python/32 signatures
pass, without original UI. 573 inputs recorded; three isolated runs are cleaned.
Whole-party channel ownership/admission, other systems and all modes remain pending.
Do not push, migrate business data, stop shared MySQL or launch original UI.
The older entries below preserve their historical scope and counts.

Read docs/connection_handoff.md and docs/evidence/connection_handoff_validation.json first.
Explicit companion v1 proves a one-use same-category (60) socket handoff;
native C54 stays eight bytes and cannot authenticate by character ID or IP.
One PlayerSession retains all leases/queues. C55 commits exclusive ownership;
pre-S9 failures restore the source, post-S9 ambiguity closes the session.
S9 on the destination and S10-before-S11 are project order, not proven original
server order. Channel isolation/cross-category/whole-party transfer remain pending.
The --handoff client adapter is explicit and tested offline only. UI stays paused.
Debug/Release each passed 12,550 C++ (handoff 611), 16 frozen batches,
5,636 TCP checks, 47 handoffs and five three-person game/queued-movement rounds.
11 IDA/PE functions, seven bounded runs (153 instructions, five adapted calls),
32 client signatures, 215 JS checks and 18 Python tests passed. Zero build
warnings/errors; 565 input hashes. All three isolated runs cleaned.
No business migration, shared MySQL shutdown, commit or push. Full goal active.
The entries below preserve the earlier increments and their historical limits.

Read docs/channel_transfer.md and docs/evidence/channel_transfer_validation.json first.
Seven native channel codecs are implemented. The current single-category server
answers authenticated C53 with native S8 result 2, preserving the source scene;
C54/C55 cannot grant identity or close another connection without a handoff.
C54 contains only character/UI DWORDs; no Passport is serialized. S9's receiving
connection sends C55, but original server routing/order is still unproven.
Actual channel migration and separately authenticated handoff remain unfinished.
IDA/PE verifies 22 functions, seven ranges, five resources and one vtable;
22 bounded native runs execute 437 instructions with 38 packet I/O adapters.
Debug/Release each passed 11,939 C++ (288 codec/auth, 329 scene), 16 frozen
batches, 4,134 TCP with 65 refusals and five three-person game rounds, MySQL
suites and 10 runtime checks. Zero warnings/errors; 558 inputs verified.
Both isolated runs cleaned; shared MySQL and original samples remain unchanged.
No UI, commit/push or business migration. Read the handoff boundary before
implementing successful channel movement; never authenticate C54 by character ID
or source IP. Full goal active. The following entries preserve historical scope.

Read docs/party_room_exit.md and docs/evidence/party_room_exit_validation.json.
Leader C404 now gathers the complete party from waiting rooms/menu/plaza into
one plaza. Source room ownership is retained until target admission; Start and
complete room removal share one registry lock. Remaining outsiders receive final
S511 notices and at most one host change. Timeout, cancellation, encoding failure,
disconnect before/after commit, mixed sources and consumed-lease reuse are covered.
Debug/Release each passed 11,322 C++ checks (489 exit, 423 prior scene), 16 frozen
batches, 3,809 authenticated TCP checks including five three-person game rounds,
MySQL suites and 10 runtime checks, with zero warnings/errors. Six native functions
match PE; no original instruction execution or UI run is claimed. All 551 inputs
verified and three isolated runs cleaned, shared MySQL preserved. Conditional
special-mode automatic C404 callers are recorded but remain unimplemented.
Direct room-to-room/channel transfer, other systems and all modes remain pending.
Do not push or run original UI; the full goal remains active. The entries below
preserve each historical increment's scope and validation counts.

Read docs/party_scene_transfers.md and docs/evidence/party_scene_validation.json.
Leader C105/C106/C402/C403 now coordinates whole-party menu/plaza moves, creation
and normal room joining. Each connection prepares/adopts its own state; reservations
guard authority until outcomes are collected. Timeout, new action, disconnect and
encoding failure have rollback coverage. Destination admission is atomic; source
plaza leases are then released individually, not a global multi-registry transaction.
Debug/Release each passed 10,833 C++ checks (423 transfer), 16 frozen batches,
3,520 authenticated TCP checks and MySQL suites. Both collective creation and
joining continue through three-person ready/load/start/movement/explicit return.
16 native functions and six resources match PE; eight original leader predicates
executed 69 instructions with no adapted calls. 549 inputs verified; all five
isolated runs cleaned, shared MySQL preserved. A failing regression exposed early
invitation availability before room adoption; pending sessions now retain the
transition flag, while adopted members remain valid targets. Final binaries and
fresh TCP reports include that fix. Current sources are menu/plaza;
collective waiting-room exit, direct cross-room/channel transfers, other systems
and all modes remain unfinished. Original UI remains paused; do not claim UI
acceptance. The following entries preserve each historical increment's scope.

Read docs/party_equipment.md and docs/evidence/party_equipment_validation.json.
S201 party appearance now uses owned inventory and authenticated character views.
Menu initialization, public consent, menu/plaza/waiting-room equipment changes,
presets and explicit refresh update a cache; busy transitions retain equipment.
Native evidence verifies ten functions and four bounded 817500 executions
(294 original instructions, 24 adapted calls); no full handler/rendering execution.
Debug/Release each passed 10,410 C++ checks (668 equipment), 16 frozen batches,
3,163 authenticated TCP checks, MySQL suites and 10 runtime smoke checks.
All four isolated runs are cleaned and 542 input hashes verified. No new codec
or migration; shared MySQL and original samples remain unchanged. Equipment
refresh is event-driven; original appearance UI and collective scene coordination
remain unverified/unfinished. Do not equate separate room consents with whole-party
transfer. The following entries preserve historical scope and validation counts.

Read docs/party_consent.md and docs/evidence/party_consent_validation.json.
Public native invitation/approved visitation now joins menu/plaza parties with
atomic scene admission. Debug/Release each passed 9,742 C++ checks (1,759 new),
16 frozen batches, 1,984 authenticated TCP checks and isolated MySQL suites.
Public party administration and individually consented room entry continue into
three-person ready/loading/start/movement/return. Lease adoption is protected
against exposing an old scene as a new invitation target. All four isolated runs
are cleaned; shared MySQL is preserved. Party equipment and collective scene
coordination remain unfinished; do not claim original UI or collective transfer.
The following entries describe historical increments and their original scope.

Read docs/room_admission.md and docs/evidence/room_admission_validation.json.
Whole-group room reservation, creation and admission are implemented; existing
single-member creation/join/invitation/visitation paths use the batch core.
All identities and slots are checked before snapshot allocation/publication.
Debug/Release each passed 7,983 C++ checks (2,160 room admission) and 16 frozen
batches, 1,252 authenticated TCP checks and isolated MySQL suites. Final binary
hashes match their TCP reports; all four isolated runs are cleaned, shared MySQL
is preserved. Public party consent, equipment and cross-session transfer remain
unimplemented; do not claim group UI or public group TCP acceptance.
The following plaza/party-operation counts are historical increments.

Read docs/plaza_admission.md and docs/evidence/plaza_admission_validation.json.
Atomic whole-group plaza admission and connection generations are implemented;
production single-member entry reuses the batch path. Debug/Release each passed
5,823 C++ checks (651 plaza admission), 16 frozen batches and 1,252 authenticated
TCP checks. Group placement was exercised internally, not through public consent.
Full party admission, equipment and cross-session scene coordination remain pending.
The following party-operation and visitation counts are historical increments.

Read docs/party_service.md and docs/evidence/party_operations_validation.json.
Native party administration and connection-owned membership are now implemented;
public party admission, trusted party equipment and collective scene transfers
remain unimplemented. Debug/Release each passed 5,172 C++ checks (1,068 party
protocol/registry and 135 session), 16 frozen batches and 1,252 authenticated
TCP checks. The TCP peers still have solo parties; do not call this group-entry
validation. Do not interpret internal JoinApproved targets as client consent.

Read README.md, docs/development.md, docs/progress.md, docs/room_configuration.md,
docs/room_invitations.md and docs/room_visitation.md. Native invitation and approved
visitation entry now connect to the existing ready/load/start/movement/return flow.
Debug/Release each passed 3,969 C++ checks (611 invitation, 566 visitation),
16 frozen batches and 1,173 authenticated TCP checks. See
docs/evidence/room_visitation_validation.json for 521 final input hashes,
isolated database cleanup and limits. No invitation/visitation migration was added.
The following visitation figures are historical. Original invitation/visitation
UI remains unverified; complete party services remain unfinished.
Also read
docs/equipment_presets.md / docs/social_features.md. The independent equipment
preset and social increments require migrations nine and ten; they have only been
applied to isolated test databases. Read docs/client_runtime.md for the migrated
Messenger/runtime adapters. They have passed offline checks only; current social
TCP tests use synthetic authenticated peers and original UI execution remains paused.
The parent workspace's protocol, evidence, security, MySQL and validation rules
still apply. Its historical Source/PascalCase layout describes the preserved old
engineering tree. In this newly fetched repository follow the existing
src/build/tests layout, snake_case filenames and Google C++ naming documented
in docs/development.md. Use responsibility-based names without project-specific
Local prefixes. Never rename Windows SDK APIs.

Client executables, resources, static_analysis, and the old Source/Tests/Tools
tree outside this checkout remain read-only reference inputs. Check the current
IDA instance and sample hash before analysis; do not assume a port is permanent.
Record VA/RVA, raw bytes, evidence and verification status in docs/evidence.

Preserve the original frozen test manifests and packet fixtures. Record deliberate
compatibility changes separately with precise reasons; do not regenerate frozen
expectations from new code. Build and run relevant tests in Debug and Release.
Database tests must use isolated databases/users; never migrate the business
database or stop shared MySQL implicitly.

Current user steering: continue code and protocol analysis; do not launch client
UI tests. Prior UI observations are historical and cannot validate this checkout.
Complete other systems before extending all game modes. The full goal remains
active until implementation and required validation are actually complete.
