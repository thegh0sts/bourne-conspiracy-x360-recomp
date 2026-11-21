# n1probe

Skullburglar's n1 asset readers (`ue3`, `matdef`: copied from skullburglar `n1/` at 67aacb28, BSD 3-Clause, see
`LICENSE.skullburglar`) with the switches The Bourne Conspiracy's packages need (UE3 v511 / licensee 39; n1 was
written for 50 Cent: Blood on the Sand, v538 / licensee 73), and a probe that reports what parses.

Changes (all behind `Package::old_layout()` = version < 538, or the licensee):
- export table entries have no ComponentMap;
- StaticMesh: no TArray<FName> after InternalVersion, no licensee-73 int in sections, no per-section TArray;
- SkeletalMesh: no "on fire" materials array (licensee 73 only), no per-bone trailer (v515+);
- textures: mips stored "in a separate file" are in the texture's source package (`<package>.xxx`), not a `.tfc`.

Run (needs Rust: https://rustup.rs):

    cargo run --release -p n1probe -- "<path to>\tbc-game\BourneGame\CookedXenon"
    cargo run --release -p n1probe -- "<path to>\tbc-game\BourneGame\CookedXenon" --all --out all.txt

It writes `n1probe_report.txt` (or `--out`).
