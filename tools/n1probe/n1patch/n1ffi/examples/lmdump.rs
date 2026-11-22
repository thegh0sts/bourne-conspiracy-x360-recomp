//! Bourne: a placed component's lightmap as n1 loads it. Prints each lightmap texture's size, format and mip count and
//! the average colour of the component's part of the atlas, and writes each texture's top mip as a PPM with that part
//! outlined in red.
//! Run from `n1/`: cargo run --release -p n1ffi --example lmdump -- <CookedXenon dir> <cache dir> <level package> <component or mesh substring> <out prefix>
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 6 {
        eprintln!("usage: lmdump <CookedXenon dir> <cache dir> <level package> <component or mesh substring> <out prefix>");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let mut found = false;
    for pl in db.level_placements(&a[3]) {
        if !pl.component.contains(&a[4]) && !pl.mesh.contains(&a[4]) {
            continue;
        }
        found = true;
        let key = format!("{}.{}", a[3], pl.component);
        println!("{key}\n  mesh {}", pl.mesh);
        let mesh = db.mesh(&pl.mesh);
        match mesh.clone() {
            Some(m) => {
                let (mut lo, mut hi) = ([f32::MAX; 2], [f32::MIN; 2]);
                for u in &m.uv1 {
                    for c in 0..2 {
                        lo[c] = lo[c].min(u[c]);
                        hi[c] = hi[c].max(u[c]);
                    }
                }
                println!("  {} vertices, lightmap uv (set 1) from ({:.3}, {:.3}) to ({:.3}, {:.3})", m.uv1.len(), lo[0], lo[1], hi[0], hi[1]);
                let overrides: Vec<&String> = pl.materials.iter().collect();
                for (k, sec) in m.sections.iter().enumerate() {
                    let mat = overrides.get(k).filter(|s| !s.is_empty()).map(|s| s.as_str()).unwrap_or(sec.material.as_str());
                    let ma = db.material(mat);
                    println!("  section {k}: {} triangles, material {mat} (class {}, blend {}, two-sided {})", sec.index_count / 3, ma.class, ma.blend, ma.two_sided);
                    match &ma.def {
                        Some(d) => {
                            println!("    diffuse {:?} emissive {:?} opacity {:?} normal {:?}", d.diffuse, d.emissive, d.opacity, d.normal);
                            for (i, n) in d.nodes.iter().enumerate() {
                                println!("    {i}: {n:?}");
                            }
                            println!("    textures {:?}", d.textures);
                        }
                        None => println!("    (no compiled material)"),
                    }
                }
            }
            None => println!("  mesh did not load"),
        }
        let Some(l) = db.component_light_full(&key) else {
            println!("  no baked light");
            continue;
        };
        if let assetdb::ComponentLight::Dir { words, scale } = l.as_ref() {
            // As n1's shader reads them: each word's bytes A,R,G,B -> (R,G,B) / 255, times the word's RGB scale.
            let nv = words.len() / 3;
            let mesh_v = mesh.as_ref().map_or(0, |m| m.uv1.len());
            println!("  directional vertex light: {nv} vertices (mesh has {mesh_v}), scales {scale:?}");
            let mut sum = [[0f64; 3]; 3];
            let mut bytes = [[0f64; 4]; 3];
            for v in 0..nv {
                for k in 0..3 {
                    let w = words[3 * v + k];
                    let b = w.to_le_bytes();
                    for c in 0..4 {
                        bytes[k][c] += b[c] as f64;
                    }
                    for c in 0..3 {
                        sum[k][c] += b[1 + c] as f64 / 255.0 * scale[k][c] as f64;
                    }
                }
            }
            for k in 0..3 {
                let n = nv.max(1) as f64;
                println!("    sample {k}: average bytes (A,R,G,B as stored) ({:.0}, {:.0}, {:.0}, {:.0}) -> scaled rgb ({:.3}, {:.3}, {:.3})",
                    bytes[k][0] / n, bytes[k][1] / n, bytes[k][2] / n, bytes[k][3] / n, sum[k][0] / n, sum[k][1] / n, sum[k][2] / n);
            }
            continue;
        }
        let assetdb::ComponentLight::Map { textures, scale, coord_scale, coord_bias } = l.as_ref() else {
            println!("  vertex light, not a lightmap or directional");
            continue;
        };
        println!("  uv scale {coord_scale:?} bias {coord_bias:?}");
        for (k, t) in textures.iter().enumerate() {
            let Some((p, i)) = t else {
                println!("  texture {k}: none");
                continue;
            };
            let Some(t) = db.lightmap_texture(p, *i) else {
                println!("  texture {k}: {p}::#{i} did not load");
                continue;
            };
            let f = match t.fmt {
                assetdb::TexFmt::Bc1 => scenekit::TexFormat::Bc1,
                assetdb::TexFmt::Bc2 => scenekit::TexFormat::Bc2,
                assetdb::TexFmt::Bc3 => scenekit::TexFormat::Bc3,
                assetdb::TexFmt::Bc5 => scenekit::TexFormat::Bc5,
                other => {
                    println!("  texture {k}: {p}::#{i} {}x{} {other:?}, {} mips (not decoded)", t.width, t.height, t.mips.len());
                    continue;
                }
            };
            let (w, h) = (t.width as usize, t.height as usize);
            let px = match n1ffi::bcdec::decode_mip(f, t.width, t.height, &t.mips[0]) {
                Some(px) => px,
                None => {
                    println!("  texture {k}: {p}::#{i} {}x{} {:?}: decode failed", t.width, t.height, t.fmt);
                    continue;
                }
            };
            let bpp = px.len() / (w * h).max(1);
            let x0 = ((coord_bias[0] * w as f32) as usize).min(w - 1);
            let y0 = ((coord_bias[1] * h as f32) as usize).min(h - 1);
            let x1 = (((coord_bias[0] + coord_scale[0]) * w as f32) as usize).clamp(x0 + 1, w);
            let y1 = (((coord_bias[1] + coord_scale[1]) * h as f32) as usize).clamp(y0 + 1, h);
            let mut sum = [0f64; 3];
            for y in y0..y1 {
                for x in x0..x1 {
                    for c in 0..3 {
                        sum[c] += px[(y * w + x) * bpp + c.min(bpp - 1)] as f64;
                    }
                }
            }
            let n = ((x1 - x0) * (y1 - y0)) as f64;
            let avg = sum.map(|s| s / n / 255.0);
            println!(
                "  texture {k}: {p}::#{i} {w}x{h} {:?} srgb {} mips {} | scale {:?} | this component's part ({x0},{y0})-({x1},{y1}) average rgb ({:.3}, {:.3}, {:.3})",
                t.fmt, t.srgb, t.mips.len(), scale[k], avg[0], avg[1], avg[2]
            );
            let mut out = format!("P6\n{w} {h}\n255\n").into_bytes();
            for y in 0..h {
                for x in 0..w {
                    let edge = (y >= y0 && y < y1 && (x == x0 || x + 1 == x1)) || (x >= x0 && x < x1 && (y == y0 || y + 1 == y1));
                    if edge {
                        out.extend([255, 0, 0]);
                    } else {
                        let o = (y * w + x) * bpp;
                        out.extend([px[o], px[o + 1.min(bpp - 1)], px[o + 2.min(bpp - 1)]]);
                    }
                }
            }
            // Every decoded mip against mip 0 shrunk to its size: lower mips that disagree mean the top mips came
            // from somewhere else (e.g. the wrong place in the _LM file).
            for (level, data) in t.mips.iter().enumerate().skip(1) {
                let (lw, lh) = ((t.width >> level).max(1), (t.height >> level).max(1));
                let Some(lp) = n1ffi::bcdec::decode_mip(f, lw, lh, data) else {
                    println!("    mip {level} {lw}x{lh}: decode failed");
                    continue;
                };
                let lb = lp.len() / (lw * lh).max(1) as usize;
                let s = 1usize << level;
                let (mut num, mut da, mut db, mut ma, mut mb) = (0f64, 0f64, 0f64, 0f64, 0f64);
                let mut pairs = Vec::new();
                for y in 0..lh as usize {
                    for x in 0..lw as usize {
                        for c in 0..3 {
                            let mut acc = 0f64;
                            for yy in 0..s {
                                for xx in 0..s {
                                    acc += px[((y * s + yy) * w + x * s + xx) * bpp + c.min(bpp - 1)] as f64;
                                }
                            }
                            let a0 = acc / (s * s) as f64;
                            let b0 = lp[(y * lw as usize + x) * lb + c.min(lb - 1)] as f64;
                            ma += a0;
                            mb += b0;
                            pairs.push((a0, b0));
                        }
                    }
                }
                let n = pairs.len().max(1) as f64;
                ma /= n;
                mb /= n;
                for (a0, b0) in &pairs {
                    num += (a0 - ma) * (b0 - mb);
                    da += (a0 - ma).powi(2);
                    db += (b0 - mb).powi(2);
                }
                let corr = num / (da.sqrt() * db.sqrt()).max(1e-9);
                println!("    mip {level} {lw}x{lh}: matches mip 0 shrunk with correlation {corr:.3} (mean {:.1} vs {:.1})", mb, ma);
                if k == 0 {
                    let mut img = format!("P6\n{lw} {lh}\n255\n").into_bytes();
                    for i in 0..(lw * lh) as usize {
                        img.extend([lp[i * lb], lp[i * lb + 1.min(lb - 1)], lp[i * lb + 2.min(lb - 1)]]);
                    }
                    let _ = std::fs::write(format!("{}_0_mip{level}.ppm", a[5]), img);
                }
            }
            if k == 0 {
                if let Some(m) = mesh.as_ref() {
                    tile_match(m, &px, bpp, w, h, *coord_scale, *coord_bias, &a[5]);
                }
            }
            let file = format!("{}_{k}.ppm", a[5]);
            std::fs::write(&file, out).unwrap();
            println!("    written {file}");
        }
    }
    if !found {
        println!("no placement of {} has a component containing {}", a[3], a[4]);
    }
}

/// Where the mesh's lightmap coordinates fall, as a mask the size of its patch, scored against every patch-sized
/// position on the page (step = patch size): the share of covered texels that are lit (non-black) and of uncovered
/// texels that are black. The real patch scores near 1 on both.
fn tile_match(m: &assetdb::MeshAsset, px: &[u8], bpp: usize, w: usize, h: usize, cs: [f32; 2], cb: [f32; 2], out: &str) {
    let tw = ((cs[0] * w as f32).round() as usize).max(1);
    let th = ((cs[1] * h as f32).round() as usize).max(1);
    let mut mask = vec![false; tw * th];
    for t in m.indices.chunks_exact(3) {
        let p: Vec<[f32; 2]> = t.iter().map(|&i| {
            let u = m.uv1.get(i as usize).copied().unwrap_or([0.0, 0.0]);
            [u[0] * tw as f32, u[1] * th as f32]
        }).collect();
        let (x0, x1) = (p.iter().map(|q| q[0]).fold(f32::MAX, f32::min), p.iter().map(|q| q[0]).fold(f32::MIN, f32::max));
        let (y0, y1) = (p.iter().map(|q| q[1]).fold(f32::MAX, f32::min), p.iter().map(|q| q[1]).fold(f32::MIN, f32::max));
        let area = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1]);
        if area.abs() < 1e-9 {
            continue;
        }
        for y in (y0.floor().max(0.0) as usize)..(y1.ceil().min(th as f32) as usize) {
            for x in (x0.floor().max(0.0) as usize)..(x1.ceil().min(tw as f32) as usize) {
                let c = [x as f32 + 0.5, y as f32 + 0.5];
                let e = |a: [f32; 2], b: [f32; 2]| (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
                let (e0, e1, e2) = (e(p[0], p[1]), e(p[1], p[2]), e(p[2], p[0]));
                if (e0 >= 0.0 && e1 >= 0.0 && e2 >= 0.0) || (e0 <= 0.0 && e1 <= 0.0 && e2 <= 0.0) {
                    mask[y * tw + x] = true;
                }
            }
        }
    }
    let covered = mask.iter().filter(|&&b| b).count();
    println!("  lightmap uv coverage: {covered} of {} texels of a {tw}x{th} patch", tw * th);
    let lit = |x: usize, y: usize| {
        let o = (y * w + x) * bpp;
        (px[o] as u32 + px[o + 1.min(bpp - 1)] as u32 + px[o + 2.min(bpp - 1)] as u32) > 12
    };
    let own = ((cb[0] * w as f32) as usize, (cb[1] * h as f32) as usize);
    let mut scores = Vec::new();
    let mut oy = 0;
    while oy + th <= h {
        let mut ox = 0;
        while ox + tw <= w {
            let (mut a, mut b, mut na, mut nb) = (0u32, 0u32, 0u32, 0u32);
            for y in 0..th {
                for x in 0..tw {
                    if mask[y * tw + x] {
                        na += 1;
                        a += lit(ox + x, oy + y) as u32;
                    } else {
                        nb += 1;
                        b += (!lit(ox + x, oy + y)) as u32;
                    }
                }
            }
            let sa = a as f32 / na.max(1) as f32;
            let sb = if nb == 0 { 1.0 } else { b as f32 / nb as f32 };
            scores.push((sa * sb, ox, oy, sa, sb));
            ox += tw;
        }
        oy += th;
    }
    scores.sort_by(|p, q| q.0.partial_cmp(&p.0).unwrap());
    for (s, ox, oy, sa, sb) in &scores {
        let mark = if (*ox as i64 - own.0 as i64).abs() <= 1 && (*oy as i64 - own.1 as i64).abs() <= 1 { "  <- the patch n1 uses" } else { "" };
        println!("    patch at ({ox},{oy}): score {s:.3} (covered lit {sa:.3}, uncovered black {sb:.3}){mark}");
    }
    let mut img = format!("P6\n{tw} {th}\n255\n").into_bytes();
    for &b in &mask {
        img.extend(if b { [255u8, 255, 255] } else { [0, 0, 0] });
    }
    let _ = std::fs::write(format!("{out}_uvmask.ppm"), img);
}
