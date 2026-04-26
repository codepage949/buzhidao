// Prevents an additional console window on Windows in release, DO NOT REMOVE
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    if buzhidao_lib::is_release_ocr_smoke_requested(std::env::args().skip(1)) {
        if let Err(error) = buzhidao_lib::run_release_ocr_smoke_from_env() {
            eprintln!("{error}");
            std::process::exit(1);
        }
        return;
    }

    buzhidao_lib::run();
}
