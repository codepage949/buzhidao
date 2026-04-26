fn main() {
    println!("cargo:rustc-check-cfg=cfg(has_paddle_inference)");
    if cfg!(target_os = "linux") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN");
    }

    if std::env::var_os("CARGO_FEATURE_GPU").is_some() {
        println!("cargo:rerun-if-changed=.cuda");
        stage_cuda_runtime_shared_libs();
    }

    if std::env::var_os("CARGO_FEATURE_PADDLE_FFI").is_some() {
        println!("cargo:rerun-if-changed=native/paddle_bridge/bridge.cc");
        println!("cargo:rerun-if-changed=native/paddle_bridge/bridge.h");
        println!("cargo:rerun-if-changed=build.rs");
        println!("cargo:rerun-if-env-changed=CARGO_FEATURE_PADDLE_FFI");

        let mut build = cc::Build::new();
        build
            .cpp(true)
            .opt_level(3)
            .file("native/paddle_bridge/bridge.cc")
            .include("native/paddle_bridge")
            .flag(if cfg!(target_env = "msvc") {
                "/std:c++17"
            } else {
                "-std=c++17"
            });
        if cfg!(target_env = "msvc") {
            build.flag("/utf-8");
            build.flag("/O2");
        } else {
            build.flag("-finput-charset=UTF-8");
            build.flag("-O3");
        }
        println!(
            "cargo:rerun-if-changed={}",
            std::path::Path::new(".paddle_inference")
                .join("third_party")
                .join("sidecar-runtime-manifest.json")
                .display()
        );
        let (clipper_include, clipper_sources, define_name) = find_pyclipper_cpp_sources()
            .expect("BUZHIDAO: sidecar와 동일한 pyclipper 1.4.0 C++ 소스를 찾지 못했습니다. tools/scripts/setup_paddle_inference.py를 먼저 실행하세요.");
        println!("cargo:rerun-if-changed={}", clipper_include.display());
        for source in &clipper_sources {
            println!("cargo:rerun-if-changed={}", source.display());
            build.file(source);
        }
        build.include(clipper_include);
        build.define(define_name, Some("1"));

        let opencv = find_opencv_sdk().expect(
            "BUZHIDAO: .paddle_inference 아래 OpenCV SDK를 찾지 못했습니다. tools/scripts/setup_paddle_inference.py를 먼저 실행하거나 --opencv-sdk-dir로 .paddle_inference/third_party/opencv-sdk/<platform>를 준비하세요."
        );
        for include_dir in &opencv.include_dirs {
            println!("cargo:rerun-if-changed={}", include_dir.display());
            build.include(include_dir);
        }
        for lib_dir in &opencv.link_search_dirs {
            println!("cargo:rustc-link-search=native={}", lib_dir.display());
        }
        for lib_name in &opencv.libs {
            println!("cargo:rustc-link-lib={}", lib_name);
        }
        build.define("BUZHIDAO_HAVE_OPENCV", Some("1"));
        for runtime_dir in &opencv.runtime_dirs {
            stage_runtime_shared_libs_from_dir(runtime_dir);
        }

        let mut has_inference_support = false;
        let candidate_dirs = default_paddle_inference_roots();
        for dir in candidate_dirs {
            match find_paddle_inference_dirs(&dir) {
                Some((include_dir, lib_dir)) => {
                    if let Some(lib_name) = find_candidate_paddle_inference_library_name(&lib_dir)
                    {
                        build.include(&include_dir);
                        build.define("BUZHIDAO_HAVE_PADDLE_INFERENCE", Some("1"));
                        println!("cargo:rustc-link-search=native={}", lib_dir.display());
                        stage_paddle_runtime_shared_libs(&dir);
                        if cfg!(target_os = "linux") {
                            if let Ok(output) = std::process::Command::new("g++")
                                .arg("-print-file-name=libstdc++.a")
                                .output()
                            {
                                let path = std::path::PathBuf::from(
                                    String::from_utf8_lossy(&output.stdout).trim(),
                                );
                                if let Some(parent) = path.parent() {
                                    println!("cargo:rustc-link-search=native={}", parent.display());
                                }
                            }
                            println!("cargo:rustc-link-lib=static=stdc++");
                        }
                        println!("cargo:rustc-link-lib={lib_name}");
                        has_inference_support = true;
                        break;
                    }
                    println!(
                        "cargo:warning=BUZHIDAO: {}에서 paddle_inference 라이브러리 파일을 찾지 못해 FFI 링크를 비활성화합니다.",
                        dir.display()
                    );
                    println!(
                        "cargo:warning=BUZHIDAO: lib 후보는 lib 폴더에서 .*_inference* 확장자(.lib/.so/.a/.dylib) 형태를 확인해야 합니다."
                    );
                }
                None => {
                    println!(
                        "cargo:warning=BUZHIDAO: {}에서 include/lib 경로를 확인할 수 없습니다. 현재 지원하는 배치: {{root}}/include+lib, {{root}}/paddle/include+lib, {{root}}/paddle_inference/include+lib",
                        dir.display()
                    );
                    if is_likely_paddle_source_tree(&dir) {
                        println!(
                            "cargo:warning=BUZHIDAO: 현재 경로는 Paddle 소스 트리로 보입니다. Paddle Inference SDK 패키지 루트(배포 zip/폴더)로 지정해야 합니다."
                        );
                    }
                }
            }
        }
        if has_inference_support {
            println!("cargo:rustc-cfg=has_paddle_inference");
        }

        build.compile("buzhidao_paddle_bridge");
    }

    tauri_build::build()
}

fn stage_cuda_runtime_shared_libs() {
    for dir in default_cuda_runtime_dirs() {
        if dir.is_dir() {
            stage_runtime_shared_libs_from_dir(&dir);
        } else {
            println!(
                "cargo:warning=BUZHIDAO: GPU 빌드지만 CUDA 런타임 디렉터리를 찾지 못했습니다: {}",
                dir.display()
            );
        }
    }
}

fn default_cuda_runtime_dirs() -> Vec<std::path::PathBuf> {
    let mut dirs = Vec::new();
    if let Some(manifest_dir) = std::env::var_os("CARGO_MANIFEST_DIR") {
        dirs.push(std::path::PathBuf::from(manifest_dir).join(".cuda"));
    }
    dirs
}

struct OpenCvSdk {
    include_dirs: Vec<std::path::PathBuf>,
    link_search_dirs: Vec<std::path::PathBuf>,
    runtime_dirs: Vec<std::path::PathBuf>,
    libs: Vec<String>,
}

fn stage_paddle_runtime_shared_libs(root: &std::path::Path) {
    let Some(_profile_dir) = cargo_profile_dir() else {
        println!(
            "cargo:warning=BUZHIDAO: OUT_DIR를 기준으로 Cargo 프로필 디렉터리를 찾지 못해 Paddle 런타임 라이브러리 복사를 건너뜁니다."
        );
        return;
    };

    let runtime_dirs = collect_paddle_runtime_library_dirs(root);
    if runtime_dirs.is_empty() {
        println!(
            "cargo:warning=BUZHIDAO: {} 아래에서 Paddle 런타임 라이브러리 디렉터리를 찾지 못했습니다.",
            root.display()
        );
        return;
    }

    for runtime_dir in runtime_dirs {
        stage_runtime_shared_libs_from_dir(&runtime_dir);
    }
}

fn stage_runtime_shared_libs_from_dir(runtime_dir: &std::path::Path) {
    let Some(profile_dir) = cargo_profile_dir() else {
        return;
    };
    let destinations = [profile_dir.clone(), profile_dir.join("deps")];
    for file in shared_library_files(runtime_dir) {
        let Some(file_name) = file.file_name() else {
            continue;
        };
        for destination in &destinations {
            if let Err(error) = std::fs::create_dir_all(destination) {
                println!(
                    "cargo:warning=BUZHIDAO: {} 디렉터리 생성 실패: {}",
                    destination.display(),
                    error
                );
                continue;
            }
            let target = destination.join(file_name);
            if let Err(error) = std::fs::copy(&file, &target) {
                println!(
                    "cargo:warning=BUZHIDAO: {} -> {} 복사 실패: {}",
                    file.display(),
                    target.display(),
                    error
                );
            }
        }
    }
}

fn cargo_profile_dir() -> Option<std::path::PathBuf> {
    let out_dir = std::env::var_os("OUT_DIR").map(std::path::PathBuf::from)?;
    out_dir.ancestors().nth(3).map(std::path::Path::to_path_buf)
}

fn collect_paddle_runtime_library_dirs(root: &std::path::Path) -> Vec<std::path::PathBuf> {
    let mut dirs = Vec::new();
    let install_root = root.join("third_party").join("install");
    let candidates = [
        root.join("lib"),
        install_root.join("mklml").join("lib"),
        install_root.join("onednn").join("lib"),
        install_root.join("openvino").join("intel64"),
    ];

    for candidate in candidates {
        if candidate.is_dir() && !dirs.contains(&candidate) {
            dirs.push(candidate);
        }
    }

    if install_root.is_dir() {
        visit_dirs(&install_root, &mut |dir| {
            if shared_library_files(dir).next().is_some() && !dirs.contains(&dir.to_path_buf()) {
                dirs.push(dir.to_path_buf());
            }
        });
    }

    dirs
}

fn visit_dirs(root: &std::path::Path, on_dir: &mut dyn FnMut(&std::path::Path)) {
    let Ok(entries) = std::fs::read_dir(root) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        on_dir(&path);
        visit_dirs(&path, on_dir);
    }
}

fn shared_library_files(dir: &std::path::Path) -> impl Iterator<Item = std::path::PathBuf> + '_ {
    std::fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .map(|entry| entry.path())
        .filter(|path| path.is_file() && is_shared_library_file(path))
}

fn is_shared_library_file(path: &std::path::Path) -> bool {
    let Some(name) = path
        .file_name()
        .map(|value| value.to_string_lossy().to_ascii_lowercase())
    else {
        return false;
    };

    name.ends_with(".dll")
        || name.ends_with(".dylib")
        || name.ends_with(".so")
        || name.contains(".so.")
}

fn add_unique_path(paths: &mut Vec<std::path::PathBuf>, candidate: std::path::PathBuf) {
    if candidate.is_dir() && !paths.contains(&candidate) {
        paths.push(candidate);
    }
}

fn shared_library_stem(path: &std::path::Path) -> Option<String> {
    let mut name = path.file_name()?.to_string_lossy().to_string();
    if let Some((prefix, _)) = name.split_once(".so.") {
        name = prefix.to_string();
    } else {
        for suffix in [".dll", ".dylib", ".so", ".a", ".lib"] {
            if let Some(stripped) = name.strip_suffix(suffix) {
                name = stripped.to_string();
                break;
            }
        }
    }
    if let Some(stripped) = name.strip_prefix("lib") {
        name = stripped.to_string();
    }
    if name.is_empty() { None } else { Some(name) }
}

fn find_pyclipper_cpp_sources(
) -> Option<(std::path::PathBuf, Vec<std::path::PathBuf>, &'static str)> {
    if let Some(manifest_dir) = std::env::var_os("CARGO_MANIFEST_DIR") {
        let bundled_root = std::path::PathBuf::from(manifest_dir)
            .join(".paddle_inference")
            .join("third_party")
            .join("pyclipper-1.4.0");
        if let Some(found) = pyclipper_sources_from_root(&bundled_root) {
            return Some(found);
        }
    }
    None
}

fn find_opencv_sdk() -> Option<OpenCvSdk> {
    let manifest_dir = std::env::var_os("CARGO_MANIFEST_DIR").map(std::path::PathBuf::from)?;
    let paddle_root = manifest_dir.join(".paddle_inference");

    let platform_root = paddle_root
        .join("third_party")
        .join("opencv-sdk")
        .join(opencv_platform_tag());
    opencv_sdk_from_root(&platform_root)
}

fn opencv_platform_tag() -> &'static str {
    if cfg!(target_os = "windows") {
        "windows-x86_64"
    } else if cfg!(target_os = "linux") {
        "linux-x86_64"
    } else if cfg!(target_os = "macos") {
        if cfg!(target_arch = "aarch64") {
            "darwin-arm64"
        } else {
            "darwin-x86_64"
        }
    } else {
        "unknown"
    }
}

fn opencv_sdk_from_root(root: &std::path::Path) -> Option<OpenCvSdk> {
    for candidate_root in [root.to_path_buf(), root.join("opencv")] {
        if let Some(sdk) = opencv_sdk_from_candidate_root(&candidate_root) {
            return Some(sdk);
        }
    }
    None
}

fn opencv_sdk_from_candidate_root(root: &std::path::Path) -> Option<OpenCvSdk> {
    let mut include_dirs = Vec::new();
    for candidate in [
        root.join("install").join("include").join("opencv4"),
        root.join("install").join("include"),
        root.join("build").join("include").join("opencv4"),
        root.join("build").join("include"),
        root.join("include").join("opencv4"),
        root.join("include"),
    ] {
        add_unique_path(&mut include_dirs, candidate);
    }
    let include_dir = include_dirs.first()?.clone();

    let mut link_search_dirs = Vec::new();
    let mut runtime_dirs = Vec::new();
    for candidate in [
        root.join("install").join("x64").join("vc17").join("lib"),
        root.join("install").join("x64").join("vc16").join("lib"),
        root.join("install").join("lib").join("Release"),
        root.join("install").join("lib64"),
        root.join("install").join("lib"),
        root.join("build").join("x64").join("vc17").join("lib"),
        root.join("build").join("x64").join("vc16").join("lib"),
        root.join("build").join("lib").join("Release"),
        root.join("build").join("lib64"),
        root.join("build").join("lib"),
        root.join("x64").join("vc17").join("lib"),
        root.join("x64").join("vc16").join("lib"),
        root.join("lib64"),
        root.join("lib"),
    ] {
        add_unique_path(&mut link_search_dirs, candidate);
    }
    for candidate in [
        root.join("install").join("x64").join("vc17").join("bin"),
        root.join("install").join("x64").join("vc16").join("bin"),
        root.join("install").join("bin").join("Release"),
        root.join("install").join("bin"),
        root.join("build").join("x64").join("vc17").join("bin"),
        root.join("build").join("x64").join("vc16").join("bin"),
        root.join("build").join("bin").join("Release"),
        root.join("build").join("bin"),
        root.join("x64").join("vc17").join("bin"),
        root.join("x64").join("vc16").join("bin"),
        root.join("bin"),
    ] {
        add_unique_path(&mut runtime_dirs, candidate);
    }

    let libs = detect_opencv_libs(&link_search_dirs)?;
    if runtime_dirs.is_empty() {
        runtime_dirs.extend(link_search_dirs.iter().cloned());
    }

    Some(OpenCvSdk {
        include_dirs: vec![include_dir],
        link_search_dirs,
        runtime_dirs,
        libs,
    })
}

fn detect_opencv_libs(link_search_dirs: &[std::path::PathBuf]) -> Option<Vec<String>> {
    for lib_dir in link_search_dirs {
        let Ok(entries) = std::fs::read_dir(lib_dir) else {
            continue;
        };
        let names: Vec<String> = entries
            .flatten()
            .filter_map(|entry| shared_library_stem(&entry.path()))
            .collect();
        if let Some(world) = names.iter().find(|name| name.starts_with("opencv_world")) {
            return Some(vec![world.clone()]);
        }
        let required = ["opencv_core", "opencv_imgproc", "opencv_imgcodecs"];
        if required.iter().all(|prefix| names.iter().any(|name| name.starts_with(prefix))) {
            return Some(
                required
                    .into_iter()
                    .filter_map(|prefix| names.iter().find(|name| name.starts_with(prefix)).cloned())
                    .collect(),
            );
        }
    }
    None
}

fn pyclipper_sources_from_root(
    root: &std::path::Path,
) -> Option<(std::path::PathBuf, Vec<std::path::PathBuf>, &'static str)> {
    let src_dir = root.join("src");
    let clipper = src_dir.join("clipper.cpp");
    let header = src_dir.join("clipper.hpp");
    let extra = src_dir.join("extra_defines.hpp");
    if src_dir.exists() && clipper.exists() && header.exists() && extra.exists() {
        Some((src_dir, vec![clipper], "BUZHIDAO_HAVE_PYCLIPPER_CLIPPER"))
    } else {
        None
    }
}

fn default_paddle_inference_roots() -> Vec<std::path::PathBuf> {
    let mut roots = Vec::new();
    if let Some(manifest_dir) = std::env::var_os("CARGO_MANIFEST_DIR") {
        let manifest_dir = std::path::PathBuf::from(manifest_dir);
        roots.push(manifest_dir.join(".paddle_inference"));
    }
    roots
}

fn find_paddle_inference_dirs(
    root: &std::path::Path,
) -> Option<(std::path::PathBuf, std::path::PathBuf)> {
    let candidates = [
        root.to_path_buf(),
        root.join("paddle"),
        root.join("paddle_inference"),
    ];
    candidates.into_iter().find_map(|candidate| {
        let include_dir = candidate.join("include");
        let lib_dir = candidate.join("lib");
        if include_dir.exists() && lib_dir.exists() {
            Some((include_dir, lib_dir))
        } else {
            None
        }
    })
}

fn is_likely_paddle_source_tree(root: &std::path::Path) -> bool {
    root.join("CMakeLists.txt").exists()
        && root.join("paddle").is_dir()
        && root.join("python").is_dir()
}

fn find_candidate_paddle_inference_library_name(lib_dir: &std::path::Path) -> Option<String> {
    std::fs::read_dir(lib_dir)
        .ok()?
        .flatten()
        .filter_map(|entry| {
            let path = entry.path();
            let name = path.file_name()?.to_string_lossy().to_ascii_lowercase();
            if !name.contains("paddle_inference") {
                return None;
            }
            if !(name.ends_with(".lib")
                || name.ends_with(".a")
                || name.ends_with(".so")
                || name.contains(".so.")
                || name.ends_with(".dylib"))
            {
                return None;
            }
            shared_library_stem(&path)
        })
        .next()
}
