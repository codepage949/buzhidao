fn main() {
    if std::env::var_os("CARGO_FEATURE_PADDLE_FFI").is_some() {
        println!("cargo:rerun-if-env-changed=PADDLE_INFERENCE_DIR");

        let mut build = cc::Build::new();
        build
            .cpp(true)
            .file("native/paddle_bridge/bridge.cc")
            .include("native/paddle_bridge")
            .flag_if_supported("-std=c++17");

        if let Some(dir) = std::env::var_os("PADDLE_INFERENCE_DIR") {
            let dir = std::path::PathBuf::from(dir);
            let include_dir = dir.join("paddle").join("include");
            let lib_dir = dir.join("paddle").join("lib");
            if include_dir.exists() && lib_dir.exists() {
                build.include(&include_dir);
                build.define("BUZHIDAO_HAVE_PADDLE_INFERENCE", Some("1"));
                println!("cargo:rustc-link-search=native={}", lib_dir.display());
                println!("cargo:rustc-link-lib=paddle_inference");
                println!("cargo:rustc-link-lib=dylib=stdc++");
            }
        }

        build.compile("buzhidao_paddle_bridge");
    }

    tauri_build::build()
}
