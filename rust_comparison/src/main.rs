use std::env;
use std::error::Error;

use tiny_skia_rust_png::write_reference_png;

fn main() -> Result<(), Box<dyn Error>> {
    let output_path = env::args()
        .nth(1)
        .unwrap_or_else(|| "tiny_skia_rust.png".to_string());

    write_reference_png(&output_path)?;
    println!("Wrote Rust reference PNG to {}", output_path);

    Ok(())
}
