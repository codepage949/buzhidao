mod cls;
mod det;
mod rec;

use image::{DynamicImage, Rgb, RgbImage};
use ort::session::Session;
use std::path::Path;
use std::sync::Mutex;

use crate::services::OcrDetection;

/// ONNX Runtime 기반 OCR 엔진.
/// det(검출) → cls(방향분류) → rec(인식) 파이프라인.
pub(crate) struct OcrEngine {
    det_session: Mutex<Session>,
    cls_session: Mutex<Session>,
    rec_session: Mutex<Session>,
    dict: Vec<String>,
}

impl OcrEngine {
    pub(crate) fn new(models_dir: &Path) -> Result<Self, String> {
        let det_session = Session::builder()
            .map_err(|e| format!("det 세션 빌더 실패: {e}"))?
            .commit_from_file(models_dir.join("det.onnx"))
            .map_err(|e| format!("det 모델 로드 실패: {e}"))?;

        let cls_session = Session::builder()
            .map_err(|e| format!("cls 세션 빌더 실패: {e}"))?
            .commit_from_file(models_dir.join("cls.onnx"))
            .map_err(|e| format!("cls 모델 로드 실패: {e}"))?;

        let rec_session = Session::builder()
            .map_err(|e| format!("rec 세션 빌더 실패: {e}"))?
            .commit_from_file(models_dir.join("rec.onnx"))
            .map_err(|e| format!("rec 모델 로드 실패: {e}"))?;

        let dict_path = models_dir.join("rec_dict.txt");
        let dict_content =
            std::fs::read_to_string(&dict_path).map_err(|e| format!("사전 파일 로드 실패: {e}"))?;
        let dict: Vec<String> = dict_content.lines().map(|s| s.to_string()).collect();

        Ok(Self {
            det_session: Mutex::new(det_session),
            cls_session: Mutex::new(cls_session),
            rec_session: Mutex::new(rec_session),
            dict,
        })
    }

    pub(crate) fn predict(
        &self,
        img: &DynamicImage,
        score_thresh: f32,
    ) -> Result<Vec<OcrDetection>, String> {
        let boxes = {
            let mut session = self.det_session.lock().unwrap();
            det::detect(&mut session, img)?
        };

        let mut detections = Vec::new();

        for box_pts in &boxes {
            let cropped = crop_box(img, box_pts);

            let label = {
                let mut session = self.cls_session.lock().unwrap();
                cls::classify(&mut session, &cropped)?
            };
            let oriented = if label == 1 {
                cropped.rotate180()
            } else {
                cropped
            };

            let (text, score) = {
                let mut session = self.rec_session.lock().unwrap();
                rec::recognize(&mut session, &oriented, &self.dict)?
            };

            if score >= score_thresh && !text.is_empty() {
                let polygon: Vec<[f64; 2]> = box_pts.iter().map(|&pt| pt).collect();
                detections.push((polygon, text));
            }
        }

        Ok(detections)
    }
}

fn crop_box(img: &DynamicImage, box_pts: &[[f64; 2]; 4]) -> DynamicImage {
    if should_use_warp_crop(box_pts) {
        warp_crop_box(img, box_pts)
    } else {
        axis_aligned_crop_box(img, box_pts)
    }
}

fn should_use_warp_crop(box_pts: &[[f64; 2]; 4]) -> bool {
    let top_dx = box_pts[1][0] - box_pts[0][0];
    let top_dy = box_pts[1][1] - box_pts[0][1];
    let bottom_dx = box_pts[2][0] - box_pts[3][0];
    let bottom_dy = box_pts[2][1] - box_pts[3][1];

    let top_angle = top_dy.atan2(top_dx).abs();
    let bottom_angle = bottom_dy.atan2(bottom_dx).abs();
    top_angle.max(bottom_angle) > 0.15
}

fn axis_aligned_crop_box(img: &DynamicImage, box_pts: &[[f64; 2]; 4]) -> DynamicImage {
    let mut min_x = f64::MAX;
    let mut min_y = f64::MAX;
    let mut max_x = f64::MIN;
    let mut max_y = f64::MIN;

    for &[x, y] in box_pts {
        min_x = min_x.min(x);
        min_y = min_y.min(y);
        max_x = max_x.max(x);
        max_y = max_y.max(y);
    }

    let x = min_x.max(0.0) as u32;
    let y = min_y.max(0.0) as u32;
    let w = ((max_x - min_x).ceil() as u32)
        .max(1)
        .min(img.width().saturating_sub(x));
    let h = ((max_y - min_y).ceil() as u32)
        .max(1)
        .min(img.height().saturating_sub(y));

    img.crop_imm(x, y, w, h)
}

fn warp_crop_box(img: &DynamicImage, box_pts: &[[f64; 2]; 4]) -> DynamicImage {
    let top_w = distance(box_pts[0], box_pts[1]);
    let bottom_w = distance(box_pts[3], box_pts[2]);
    let left_h = distance(box_pts[0], box_pts[3]);
    let right_h = distance(box_pts[1], box_pts[2]);

    let out_w = ((top_w + bottom_w) * 0.5).round().max(1.0) as u32;
    let out_h = ((left_h + right_h) * 0.5).round().max(1.0) as u32;

    let src = img.to_rgb8();
    let mut out = RgbImage::new(out_w, out_h);
    let dst = [
        [0.0, 0.0],
        [out_w as f64, 0.0],
        [out_w as f64, out_h as f64],
        [0.0, out_h as f64],
    ];
    let homography = solve_homography(&dst, box_pts).unwrap_or([
        [1.0, 0.0, 0.0],
        [0.0, 1.0, 0.0],
        [0.0, 0.0, 1.0],
    ]);

    for y in 0..out_h {
        for x in 0..out_w {
            let [src_x, src_y] = project_point(&homography, x as f64, y as f64);

            out.put_pixel(x, y, sample_bilinear(&src, src_x, src_y));
        }
    }

    DynamicImage::ImageRgb8(out)
}

#[cfg(test)]
fn order_box_points(box_pts: &[[f64; 2]; 4]) -> [[f64; 2]; 4] {
    let mut pts = *box_pts;
    let cx = pts.iter().map(|p| p[0]).sum::<f64>() / 4.0;
    let cy = pts.iter().map(|p| p[1]).sum::<f64>() / 4.0;

    pts.sort_by(|a, b| {
        let aa = (a[1] - cy).atan2(a[0] - cx);
        let bb = (b[1] - cy).atan2(b[0] - cx);
        aa.partial_cmp(&bb).unwrap()
    });

    let start = pts
        .iter()
        .enumerate()
        .min_by(|(_, a), (_, b)| {
            (a[0] + a[1])
                .partial_cmp(&(b[0] + b[1]))
                .unwrap()
        })
        .map(|(idx, _)| idx)
        .unwrap_or(0);

    let rotated = [
        pts[start],
        pts[(start + 1) % 4],
        pts[(start + 2) % 4],
        pts[(start + 3) % 4],
    ];

    if rotated[1][1] > rotated[3][1] {
        [rotated[0], rotated[3], rotated[2], rotated[1]]
    } else {
        rotated
    }
}

fn distance(a: [f64; 2], b: [f64; 2]) -> f64 {
    let dx = a[0] - b[0];
    let dy = a[1] - b[1];
    (dx * dx + dy * dy).sqrt()
}

fn sample_bilinear(img: &RgbImage, x: f64, y: f64) -> Rgb<u8> {
    let max_x = img.width().saturating_sub(1) as f64;
    let max_y = img.height().saturating_sub(1) as f64;
    let x = x.clamp(0.0, max_x);
    let y = y.clamp(0.0, max_y);

    let x0 = x.floor() as u32;
    let y0 = y.floor() as u32;
    let x1 = (x0 + 1).min(img.width().saturating_sub(1));
    let y1 = (y0 + 1).min(img.height().saturating_sub(1));
    let fx = x - x0 as f64;
    let fy = y - y0 as f64;

    let p00 = img.get_pixel(x0, y0);
    let p10 = img.get_pixel(x1, y0);
    let p11 = img.get_pixel(x1, y1);
    let p01 = img.get_pixel(x0, y1);

    let mut out = [0u8; 3];
    for c in 0..3 {
        let v00 = p00[c] as f64;
        let v10 = p10[c] as f64;
        let v11 = p11[c] as f64;
        let v01 = p01[c] as f64;
        let val = v00 * (1.0 - fx) * (1.0 - fy)
            + v10 * fx * (1.0 - fy)
            + v11 * fx * fy
            + v01 * (1.0 - fx) * fy;
        out[c] = val.round().clamp(0.0, 255.0) as u8;
    }

    Rgb(out)
}

fn solve_homography(src: &[[f64; 2]; 4], dst: &[[f64; 2]; 4]) -> Option<[[f64; 3]; 3]> {
    let mut a = [[0.0f64; 9]; 8];
    for i in 0..4 {
        let x = src[i][0];
        let y = src[i][1];
        let u = dst[i][0];
        let v = dst[i][1];

        a[2 * i] = [x, y, 1.0, 0.0, 0.0, 0.0, -u * x, -u * y, u];
        a[2 * i + 1] = [0.0, 0.0, 0.0, x, y, 1.0, -v * x, -v * y, v];
    }

    for col in 0..8 {
        let mut pivot = col;
        for row in col + 1..8 {
            if a[row][col].abs() > a[pivot][col].abs() {
                pivot = row;
            }
        }
        if a[pivot][col].abs() < 1e-9 {
            return None;
        }
        if pivot != col {
            a.swap(pivot, col);
        }

        let div = a[col][col];
        for k in col..9 {
            a[col][k] /= div;
        }

        for row in 0..8 {
            if row == col {
                continue;
            }
            let factor = a[row][col];
            for k in col..9 {
                a[row][k] -= factor * a[col][k];
            }
        }
    }

    let h = [
        [a[0][8], a[1][8], a[2][8]],
        [a[3][8], a[4][8], a[5][8]],
        [a[6][8], a[7][8], 1.0],
    ];
    Some(h)
}

fn project_point(h: &[[f64; 3]; 3], x: f64, y: f64) -> [f64; 2] {
    let denom = h[2][0] * x + h[2][1] * y + h[2][2];
    if denom.abs() < 1e-9 {
        return [x, y];
    }
    [
        (h[0][0] * x + h[0][1] * y + h[0][2]) / denom,
        (h[1][0] * x + h[1][1] * y + h[1][2]) / denom,
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{Rgb, RgbImage};
    use std::path::PathBuf;

    fn models_dir() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("models")
    }

    fn models_available() -> bool {
        let dir = models_dir();
        dir.join("det.onnx").exists()
            && dir.join("cls.onnx").exists()
            && dir.join("rec.onnx").exists()
            && dir.join("rec_dict.txt").exists()
    }

    #[test]
    fn 모델_로드_및_세션_초기화() {
        if !models_available() {
            eprintln!("ONNX 모델 파일 없음 — 건너뜀");
            return;
        }
        let engine = OcrEngine::new(&models_dir());
        assert!(engine.is_ok(), "OcrEngine 초기화 실패: {:?}", engine.err());
    }

    #[test]
    fn 테스트_이미지_추론() {
        if !models_available() {
            eprintln!("ONNX 모델 파일 없음 — 건너뜀");
            return;
        }

        let test_img_path =
            PathBuf::from(env!("CARGO_MANIFEST_DIR")).with_file_name("ocr/test.png");
        if !test_img_path.exists() {
            eprintln!("테스트 이미지 없음: {test_img_path:?} — 건너뜀");
            return;
        }

        let engine = OcrEngine::new(&models_dir()).expect("OcrEngine 초기화 실패");
        let img = image::open(&test_img_path).expect("테스트 이미지 로드 실패");
        let result = engine.predict(&img, 0.5);

        match &result {
            Ok(detections) => {
                eprintln!("검출 결과: {} 개 영역", detections.len());
                for (poly, text) in detections {
                    eprintln!("  텍스트: {text:?}, 폴리곤: {poly:?}");
                }
            }
            Err(e) => {
                panic!("추론 실패: {e}");
            }
        }

        assert!(result.is_ok());
    }

    #[test]
    fn 중국어_이미지_추론_비교() {
        if !models_available() {
            eprintln!("ONNX 모델 파일 없음 — 건너뜀");
            return;
        }

        let test_img_path =
            PathBuf::from(env!("CARGO_MANIFEST_DIR")).with_file_name("test.jpg");
        if !test_img_path.exists() {
            eprintln!("테스트 이미지 없음: {test_img_path:?} — 건너뜀");
            return;
        }

        let engine = OcrEngine::new(&models_dir()).expect("OcrEngine 초기화 실패");
        let img = image::open(&test_img_path).expect("테스트 이미지 로드 실패");
        let result = engine.predict(&img, 0.5);

        match &result {
            Ok(detections) => {
                eprintln!("검출 결과: {} 개 영역", detections.len());
                for (poly, text) in detections {
                    eprintln!("  텍스트: {text:?}, 폴리곤: {poly:?}");
                }
                // Python 참조: 43개 박스, 38개 텍스트(score>=0.5)
                // Rust 후처리가 다르므로 정확히 같진 않지만, 최소 20개 이상이어야 함
                assert!(
                    detections.len() >= 20,
                    "검출 수 부족: {} (최소 20개 기대)",
                    detections.len()
                );
            }
            Err(e) => {
                panic!("추론 실패: {e}");
            }
        }
    }

    #[test]
    fn 점_정렬이_좌상단부터_시계방향이_된다() {
        let pts = [[40.0, 20.0], [10.0, 10.0], [45.0, 35.0], [15.0, 30.0]];
        let ordered = order_box_points(&pts);

        assert_eq!(ordered[0], [10.0, 10.0]);
        assert_eq!(ordered[1], [40.0, 20.0]);
        assert_eq!(ordered[2], [45.0, 35.0]);
        assert_eq!(ordered[3], [15.0, 30.0]);
    }

    #[test]
    fn 보정_crop은_축정렬_영역을_보존한다() {
        let mut img = RgbImage::from_pixel(8, 8, Rgb([0, 0, 0]));
        for y in 2..6 {
            for x in 1..5 {
                img.put_pixel(x, y, Rgb([200, 100, 50]));
            }
        }

        let cropped = warp_crop_box(
            &DynamicImage::ImageRgb8(img),
            &[[1.0, 2.0], [4.0, 2.0], [4.0, 5.0], [1.0, 5.0]],
        )
        .to_rgb8();

        assert_eq!(cropped.width(), 3);
        assert_eq!(cropped.height(), 3);
        assert_eq!(*cropped.get_pixel(1, 1), Rgb([200, 100, 50]));
    }

    #[test]
    fn 수평_박스는_axis_aligned_crop을_사용한다() {
        let pts = [[1.0, 2.0], [10.0, 2.5], [10.0, 5.0], [1.0, 4.5]];
        assert!(!should_use_warp_crop(&pts));
    }

    #[test]
    fn 기울어진_박스는_warp_crop을_사용한다() {
        let pts = [[2.0, 2.0], [8.0, 4.0], [7.0, 8.0], [1.0, 6.0]];
        assert!(should_use_warp_crop(&pts));
    }

    #[test]
    fn homography는_사각형_대응점을_보존한다() {
        let src = [[0.0, 0.0], [10.0, 0.0], [10.0, 4.0], [0.0, 4.0]];
        let dst = [[2.0, 3.0], [12.0, 5.0], [11.0, 9.0], [1.0, 7.0]];
        let h = solve_homography(&src, &dst).expect("homography 계산 실패");

        for i in 0..4 {
            let p = project_point(&h, src[i][0], src[i][1]);
            assert!((p[0] - dst[i][0]).abs() < 1e-6);
            assert!((p[1] - dst[i][1]).abs() < 1e-6);
        }
    }

}
