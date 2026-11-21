//! n1probe: runs Skullburglar's n1 asset readers (ue3 crate, patched for UE3 v511 / licensee 39) over The Bourne
//! Conspiracy's cooked packages and reports what parses. Offline: no game, no GPU.
//!
//!   n1probe <CookedXenon dir> [package names or globs...] [--all] [--out report.txt]
//!
//! With no package names it probes a fixed sample (a map, a cutscene, characters); --all probes every .xxx file.

use std::collections::{BTreeMap, HashMap};
use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use ue3::Package;

#[derive(Default)]
struct Tally {
    ok: usize,
    fail: usize,
    errors: Vec<String>,
}

impl Tally {
    fn add<T, E: std::fmt::Display>(&mut self, what: &str, r: Result<T, E>) -> Option<T> {
        match r {
            Ok(v) => {
                self.ok += 1;
                Some(v)
            }
            Err(e) => {
                self.fail += 1;
                if self.errors.len() < 6 {
                    self.errors.push(format!("{what}: {e:#}"));
                }
                None
            }
        }
    }
}

#[derive(Default)]
struct Totals {
    packages_ok: usize,
    packages_fail: Vec<String>,
    classes: BTreeMap<String, usize>,
    tallies: BTreeMap<&'static str, Tally>,
    static_tris: u64,
    skel_verts: u64,
    skel_issues: usize,
    tex_mips: usize,
    tex_notes: BTreeMap<String, usize>,
    placements: usize,
    vertex_lit: usize,
    mat_missing: usize,
    mat_unsupported: usize,
    unsupported: BTreeMap<String, usize>,
    // Textures whose every mip is flagged unused (0x20): a stub; the data is in a copy cooked elsewhere.
    tex_stubs: Vec<String>,
    tex_full: std::collections::HashSet<String>,
    trailing_static: BTreeMap<usize, usize>,
    trailing_skel: BTreeMap<usize, usize>,
}

const SAMPLE: &[&str] = &[
    "ZUR_AIR_AirSide_View_m.xxx",
    "FRM_HSE_FightIntro_CINE_m.xxx",
    "MAR_DOK_TutFight_Design_m.xxx",
    "CHR_Azar_p.xxx",
    "CHR_Bourne_Jacket_p.xxx",
    "MAR_BAR_p.xxx",
    "Engine.xxx",
];

fn main() -> anyhow::Result<()> {
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    let mut out_path = PathBuf::from("n1probe_report.txt");
    if let Some(i) = args.iter().position(|a| a == "--out") {
        out_path = PathBuf::from(args.get(i + 1).cloned().unwrap_or_default());
        args.drain(i..(i + 2).min(args.len()));
    }
    let all = args.iter().any(|a| a == "--all");
    args.retain(|a| a != "--all");
    anyhow::ensure!(!args.is_empty(), "usage: n1probe <CookedXenon dir> [package...] [--all] [--out report.txt]");
    let dir = PathBuf::from(&args[0]);
    let mut files: Vec<PathBuf> = Vec::new();
    if all {
        for e in std::fs::read_dir(&dir)? {
            let p = e?.path();
            if p.extension().is_some_and(|x| x.eq_ignore_ascii_case("xxx")) {
                files.push(p);
            }
        }
        files.sort();
    } else if args.len() > 1 {
        files.extend(args[1..].iter().map(|a| dir.join(a)));
    } else {
        files.extend(SAMPLE.iter().map(|a| dir.join(a)));
    }
    let tfc = ue3::texture::TfcCache::new(&dir);
    let mut t = Totals::default();
    let mut report = String::new();
    for (n, f) in files.iter().enumerate() {
        if all && n % 50 == 0 {
            eprintln!("{n}/{} {}", files.len(), f.display());
        }
        probe(f, &tfc, &mut t, &mut report, !all);
    }
    summary(&t, &mut report);
    print!("{report}");
    std::fs::write(&out_path, &report)?;
    eprintln!("report written to {}", out_path.display());
    Ok(())
}

fn probe(path: &Path, tfc: &ue3::texture::TfcCache, t: &mut Totals, report: &mut String, verbose: bool) {
    let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let pkg = match Package::open(path) {
        Ok(p) => p,
        Err(e) => {
            t.packages_fail.push(format!("{name}: {e:#}"));
            return;
        }
    };
    t.packages_ok += 1;
    let hist = pkg.class_histogram();
    for (c, n) in &hist {
        *t.classes.entry(c.clone()).or_default() += n;
    }
    if verbose {
        let _ = writeln!(report, "== {name}: v{} / licensee {}, {} exports, {} names", pkg.version, pkg.licensee,
                         pkg.exports.len(), pkg.names.len());
        let top: Vec<String> = hist.iter().take(12).map(|(c, n)| format!("{c} {n}")).collect();
        let _ = writeln!(report, "   classes: {}", top.join(", "));
    }
    let idx_of: Vec<i32> = (1..=pkg.exports.len() as i32).collect();
    for &i in &idx_of {
        let class = pkg.class_name(i).to_string();
        let what = format!("{name} {}", pkg.obj_path(i));
        match class.as_str() {
            "StaticMesh" => {
                if let Some(m) = t.tallies.entry("StaticMesh").or_default().add(&what, ue3::mesh::read_static_mesh(&pkg, i)) {
                    t.static_tris += m.lods.first().map(|l| l.indices.len() as u64 / 3).unwrap_or(0);
                    *t.trailing_static.entry(m.trailing).or_default() += 1;
                }
            }
            "SkeletalMesh" => {
                if let Some(m) = t.tallies.entry("SkeletalMesh").or_default().add(&what, ue3::skel::read_skeletal_mesh(&pkg, i)) {
                    t.skel_verts += m.lods.first().map(|l| l.verts.len() as u64).unwrap_or(0);
                    *t.trailing_skel.entry(m.trailing).or_default() += 1;
                    if !m.lods.is_empty() {
                        let issues = m.validate(0);
                        if !issues.is_empty() {
                            t.skel_issues += 1;
                            if verbose {
                                let _ = writeln!(report, "   skeletal mesh {what}: {}", issues.join("; "));
                            }
                        }
                    }
                }
            }
            "Texture2D" | "LightMapTexture2D" | "ShadowMapTexture2D" | "TextureFlipBook" => {
                // Full path with the package for top-level objects, to match copies cooked into other packages.
                let p = pkg.obj_path(i);
                let full = if p.contains('.') { p } else { format!("{}.{p}", pkg.name) };
                let res = ue3::texture::read_texture(&pkg, i, tfc);
                if let Err(e) = &res {
                    if format!("{e:#}").contains("no usable mips ([])") {
                        t.tex_stubs.push(full);
                        continue;
                    }
                }
                if res.is_ok() {
                    t.tex_full.insert(full.to_lowercase());
                }
                if let Some(tex) = t.tallies.entry("Texture2D").or_default().add(&what, res) {
                    t.tex_mips += tex.mips.len();
                    for n in &tex.notes {
                        let key: String = n.split(|c: char| c.is_ascii_digit()).next().unwrap_or("").trim().to_string();
                        *t.tex_notes.entry(key).or_default() += 1;
                    }
                }
            }
            _ => {}
        }
    }
    // Materials, resolved within this package (a parent in another package counts as "outside").
    let mats: HashMap<String, i32> = idx_of
        .iter()
        .filter(|&&i| matches!(pkg.class_name(i), "Material" | "MaterialInstanceConstant"))
        .map(|&i| (pkg.obj_path(i), i))
        .collect();
    for (p, _) in &mats {
        let r = ue3::matir::build_by(p, |q| mats.get(q).map(|&e| (&pkg, e)));
        let tally = t.tallies.entry("Material").or_default();
        match r {
            Some(def) => {
                tally.ok += 1;
                if !def.unsupported.is_empty() {
                    t.mat_unsupported += 1;
                    for c in &def.unsupported {
                        *t.unsupported.entry(c.clone()).or_default() += 1;
                    }
                }
            }
            None => {
                // Distinguish a parent outside the package from a material that failed to compile.
                if pkg.class_name(mats[p]) == "MaterialInstanceConstant" {
                    t.mat_missing += 1;
                } else {
                    tally.fail += 1;
                    if tally.errors.len() < 6 {
                        tally.errors.push(format!("{name} {p}: material did not compile"));
                    }
                }
            }
        }
    }
    let pl = ue3::level::placements(&pkg);
    t.placements += pl.len();
    t.vertex_lit += pl.iter().filter(|(p, _)| p.vertex_light.is_some()).count();
    if verbose {
        let _ = writeln!(report, "   static mesh placements: {} ({} with baked vertex light)", pl.len(),
                         pl.iter().filter(|(p, _)| p.vertex_light.is_some()).count());
    }
}

fn summary(t: &Totals, out: &mut String) {
    let _ = writeln!(out, "\n==== summary ====");
    let _ = writeln!(out, "packages opened: {}, failed: {}", t.packages_ok, t.packages_fail.len());
    for e in t.packages_fail.iter().take(10) {
        let _ = writeln!(out, "   {e}");
    }
    for (k, v) in &t.tallies {
        let _ = writeln!(out, "{k}: {} ok, {} failed", v.ok, v.fail);
        for e in &v.errors {
            let _ = writeln!(out, "   {e}");
        }
    }
    let _ = writeln!(out, "materials whose parent is in another package (not tried): {}", t.mat_missing);
    let _ = writeln!(out, "materials using an expression n1 doesn't handle: {} ({:?})", t.mat_unsupported, t.unsupported);
    let found = t.tex_stubs.iter().filter(|s| t.tex_full.contains(&s.to_lowercase())).count();
    let _ = writeln!(out, "texture stubs (no mip data here): {}, of them with a full copy in another package: {}",
                     t.tex_stubs.len(), found);
    for s in t.tex_stubs.iter().filter(|s| !t.tex_full.contains(&s.to_lowercase())).take(10) {
        let _ = writeln!(out, "   stub without a full copy: {s}");
    }
    let _ = writeln!(out, "static mesh LOD0 triangles: {}; bytes left after the LODs: {:?}", t.static_tris, t.trailing_static);
    let _ = writeln!(out, "skeletal mesh LOD0 vertices: {}; with validation issues: {}; bytes left: {:?}", t.skel_verts,
                     t.skel_issues, t.trailing_skel);
    let _ = writeln!(out, "texture mips decoded: {}; notes: {:?}", t.tex_mips, t.tex_notes);
    let _ = writeln!(out, "static mesh placements: {} ({} with baked vertex light)", t.placements, t.vertex_lit);
    let mut classes: Vec<_> = t.classes.iter().collect();
    classes.sort_by(|a, b| b.1.cmp(a.1));
    let _ = writeln!(out, "most common classes: {}",
                     classes.iter().take(40).map(|(c, n)| format!("{c} {n}")).collect::<Vec<_>>().join(", "));
    let customs: Vec<String> = classes.iter().filter(|(c, _)| c.starts_with("Hm")).map(|(c, n)| format!("{c} {n}")).collect();
    let _ = writeln!(out, "High Moon classes (Hm*): {}", customs.join(", "));
}
