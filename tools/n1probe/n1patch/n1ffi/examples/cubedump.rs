//! Bourne: materials that sample a TextureCube (`MatDef::cubes`) and whose path contains any of the given words
//! (case-insensitive; `*` = all): node list, cube faces and sizes, and the WGSL n1 compiles. Run from `n1/`:
//!   cargo run --release -p n1ffi --example cubedump -- <CookedXenon dir> <cache dir> word [word...]
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: cubedump <CookedXenon dir> <cache dir> word [word...]");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let words: Vec<String> = a[3..].iter().map(|w| w.to_lowercase()).collect();
    let (mut seen, mut with_cubes) = (0, 0);
    for path in db.paths_of_classes(&["Material", "MaterialInstanceConstant", "MaterialInstanceTimeVarying", "DecalMaterial"]) {
        let l = path.to_lowercase();
        if !words.iter().any(|w| w == "*" || l.contains(w)) {
            continue;
        }
        seen += 1;
        let m = db.material(&path);
        let Some(d) = &m.def else { continue };
        if d.cubes.is_empty() {
            println!("{path} [{}]: no cube", d.blend);
            continue;
        }
        with_cubes += 1;
        println!("==== {path} [{}] cubes {:?} unsupported {:?}", d.blend, d.cubes, d.unsupported);
        for (i, t) in d.textures.iter().enumerate() {
            let tag = if d.cubes.contains(&(i as u32)) { "CUBE" } else { "2d" };
            match db.texture(&t.path) {
                Some(x) => println!("  t{i} {tag} {} {}x{} {:?} srgb={} mips={}", t.path, x.width, x.height, x.fmt, x.srgb, x.mips.len()),
                None => println!("  t{i} {tag} {} (not loaded)", t.path),
            }
            if d.cubes.contains(&(i as u32)) {
                println!("     faces {:?}", db.cube_face_paths(&t.path));
            }
        }
        for (i, n) in d.nodes.iter().enumerate() {
            println!("  {i}: {n:?}");
        }
        println!("  diffuse {:?} emissive {:?} specular {:?} opacity {:?} normal {:?}", d.diffuse, d.emissive, d.specular, d.opacity, d.normal);
        let n = d.textures.len();
        let clamp: Vec<u8> =
            d.textures.iter().map(|t| db.texture_meta(&t.path).map_or(0, |m| assetdb::clamp_bits(m.addr))).collect();
        let c = matdef::codegen::compile_addr(d, &vec![None; n], &vec![false; n], &vec![false; n], &clamp);
        println!("{}", c.body);
    }
    println!("{seen} materials matched, {with_cubes} with cubes");
}
