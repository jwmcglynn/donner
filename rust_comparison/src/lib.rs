use std::error::Error;
use std::fs;
use std::ptr;

use tiny_skia::{
    BlendMode, Color, FillRule, GradientStop, LineCap, LineJoin, LinearGradient, Paint, PathBuilder,
    Pixmap, Point, Shader, SpreadMode, Stroke, Transform,
};

const WIDTH: u32 = 192;
const HEIGHT: u32 = 192;

fn build_scene(pixmap: &mut Pixmap) -> Result<(), Box<dyn Error>> {
    pixmap.fill(Color::from_rgba8(18, 18, 22, 255));

    let mut builder = PathBuilder::new();
    builder.move_to(36.0, 36.0);
    builder.line_to(156.0, 48.0);
    builder.cubic_to(160.0, 84.0, 108.0, 144.0, 52.0, 156.0);
    builder.close();
    let path = builder.finish().ok_or("failed to finish path")?;

    let shader = LinearGradient::new(
        Point::from_xy(20.0, 24.0),
        Point::from_xy(172.0, 172.0),
        vec![
            GradientStop::new(0.0, Color::from_rgba8(44, 176, 255, 255)),
            GradientStop::new(1.0, Color::from_rgba8(244, 108, 92, 255)),
        ],
        SpreadMode::Reflect,
        Transform::from_rotate(0.35),
    )
    .ok_or("failed to create gradient shader")?;

    let mut fill_paint = Paint::default();
    fill_paint.shader = shader;
    fill_paint.anti_alias = true;
    fill_paint.blend_mode = BlendMode::SourceOver;

    pixmap.fill_path(&path, &fill_paint, FillRule::Winding, Transform::identity(), None);

    let mut stroke_paint = Paint::default();
    stroke_paint.shader = Shader::SolidColor(Color::from_rgba8(250, 250, 252, 255));
    stroke_paint.anti_alias = true;
    stroke_paint.blend_mode = BlendMode::SourceOver;

    let mut stroke = Stroke::default();
    stroke.width = 6.0;
    stroke.line_join = LineJoin::Round;
    stroke.line_cap = LineCap::Round;

    pixmap.stroke_path(
        &path,
        &stroke_paint,
        &stroke,
        Transform::from_translate(4.0, 6.0),
        None,
    );

    Ok(())
}

fn render_scene() -> Result<Pixmap, Box<dyn Error>> {
    let mut pixmap = Pixmap::new(WIDTH, HEIGHT).ok_or("failed to allocate pixmap")?;
    build_scene(&mut pixmap)?;
    Ok(pixmap)
}

pub fn write_reference_png(path: &str) -> Result<(), Box<dyn Error>> {
    let pixmap = render_scene()?;
    let png = pixmap.encode_png()?;
    fs::write(path, png)?;
    Ok(())
}

#[no_mangle]
pub extern "C" fn tiny_skia_rust_reference_width() -> u32 {
    WIDTH
}

#[no_mangle]
pub extern "C" fn tiny_skia_rust_reference_height() -> u32 {
    HEIGHT
}

#[no_mangle]
pub extern "C" fn tiny_skia_rust_reference_stride() -> usize {
    (WIDTH as usize) * 4
}

#[no_mangle]
pub extern "C" fn tiny_skia_rust_render_reference(buffer: *mut u8, buffer_len: usize) -> bool {
    let expected_len = tiny_skia_rust_reference_stride() * (HEIGHT as usize);
    if buffer.is_null() || buffer_len < expected_len {
        return false;
    }

    let pixmap = match render_scene() {
        Ok(pixmap) => pixmap,
        Err(_) => return false,
    };

    let data = pixmap.data();
    if data.len() != expected_len {
        return false;
    }

    unsafe {
        ptr::copy_nonoverlapping(data.as_ptr(), buffer, expected_len);
    }

    true
}
