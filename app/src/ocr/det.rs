use image::DynamicImage;
use ndarray::{Array3, Array4};
use ort::session::Session;

/// DB 후처리 파라미터 (inference.yml 기반)
const THRESH: f32 = 0.3;
const BOX_THRESH: f32 = 0.6;
const UNCLIP_RATIO: f32 = 1.5;
const MAX_CANDIDATES: usize = 1000;
const MIN_SIZE: f32 = 3.0;

/// 이미지 정규화 파라미터
const MEAN: [f32; 3] = [0.485, 0.456, 0.406];
const STD: [f32; 3] = [0.229, 0.224, 0.225];
const SCALE: f32 = 1.0 / 255.0;

/// 검출 결과: 4점 폴리곤 좌표
pub(crate) type DetBox = [[f64; 2]; 4];

/// 이미지를 32 배수 크기로 리사이즈한다 (resize_long=960).
fn resize_for_det(img: &DynamicImage) -> (DynamicImage, f64, f64) {
    let (w, h) = (img.width(), img.height());
    let resize_long = 960u32;

    let ratio = if h > w {
        resize_long as f64 / h as f64
    } else {
        resize_long as f64 / w as f64
    };

    let mut new_h = (h as f64 * ratio) as u32;
    let mut new_w = (w as f64 * ratio) as u32;

    // 128 배수로 올림
    let stride = 128u32;
    new_h = ((new_h + stride - 1) / stride) * stride;
    new_w = ((new_w + stride - 1) / stride) * stride;

    let resized = img.resize_exact(new_w, new_h, image::imageops::FilterType::Lanczos3);
    let ratio_h = new_h as f64 / h as f64;
    let ratio_w = new_w as f64 / w as f64;

    (resized, ratio_h, ratio_w)
}

/// BGR 순서로 정규화 + CHW 텐서 변환
fn preprocess(img: &DynamicImage) -> Array4<f32> {
    let rgb = img.to_rgb8();
    let (w, h) = (rgb.width() as usize, rgb.height() as usize);

    let mut tensor = Array3::<f32>::zeros((3, h, w));
    for y in 0..h {
        for x in 0..w {
            let pixel = rgb.get_pixel(x as u32, y as u32);
            // BGR 순서 (PaddleOCR det는 BGR 입력)
            tensor[[0, y, x]] = (pixel[2] as f32 * SCALE - MEAN[2]) / STD[2]; // B
            tensor[[1, y, x]] = (pixel[1] as f32 * SCALE - MEAN[1]) / STD[1]; // G
            tensor[[2, y, x]] = (pixel[0] as f32 * SCALE - MEAN[0]) / STD[0]; // R
        }
    }

    tensor.insert_axis(ndarray::Axis(0))
}

/// DB 후처리: 히트맵에서 텍스트 박스를 추출한다.
fn db_postprocess(pred: &[f32], pred_h: usize, pred_w: usize, src_h: u32, src_w: u32) -> Vec<DetBox> {
    // 이진화
    let bitmap: Vec<u8> = pred.iter().map(|&v| if v > THRESH { 1u8 } else { 0u8 }).collect();

    // 연결 컴포넌트 찾기 (간단한 flood fill 기반)
    let contours = find_contours(&bitmap, pred_h, pred_w);

    let w_scale = src_w as f64 / pred_w as f64;
    let h_scale = src_h as f64 / pred_h as f64;

    let mut boxes = Vec::new();

    for contour in contours.iter().take(MAX_CANDIDATES) {
        if contour.len() < 4 {
            continue;
        }

        // 최소 경계 사각형
        let rect = min_area_rect(contour);
        let sside = rect.min_side;
        if sside < MIN_SIZE {
            continue;
        }

        // 박스 점수
        let score = box_score_fast(pred, pred_h, pred_w, &rect.points);
        if score < BOX_THRESH {
            continue;
        }

        // unclip
        let expanded = unclip(&rect.points, UNCLIP_RATIO);
        let expanded_rect = min_area_rect(&expanded);
        if expanded_rect.min_side < MIN_SIZE + 2.0 {
            continue;
        }

        // 원본 좌표로 변환
        let mut det_box: DetBox = [[0.0; 2]; 4];
        for (i, pt) in expanded_rect.points.iter().enumerate().take(4) {
            det_box[i][0] = (pt[0] as f64 * w_scale).max(0.0).min(src_w as f64);
            det_box[i][1] = (pt[1] as f64 * h_scale).max(0.0).min(src_h as f64);
        }
        boxes.push(det_box);
    }

    boxes
}

/// ONNX 추론 실행
pub(crate) fn detect(session: &mut Session, img: &DynamicImage) -> Result<Vec<DetBox>, String> {
    let (src_w, src_h) = (img.width(), img.height());
    let (resized, _ratio_h, _ratio_w) = resize_for_det(img);
    let input = preprocess(&resized);

    let pred_h = resized.height() as usize;
    let pred_w = resized.width() as usize;

    let input_values = ort::value::Value::from_array(input).map_err(|e| e.to_string())?;
    let outputs = session.run(ort::inputs![input_values]).map_err(|e| e.to_string())?;

    let (_shape, data) = outputs[0]
        .try_extract_tensor::<f32>()
        .map_err(|e| format!("det 출력 추출 실패: {e}"))?;
    let pred: Vec<f32> = data.to_vec();

    // 출력은 [1, 1, H, W] 형태
    Ok(db_postprocess(&pred, pred_h, pred_w, src_h, src_w))
}

// ── 기하 유틸리티 ────────────────────────────────────────────────────────────

struct MinAreaRect {
    points: Vec<[f32; 2]>,
    min_side: f32,
}

fn min_area_rect(points: &[[f32; 2]]) -> MinAreaRect {
    // 간소화된 회전 경계 상자: 축 정렬 바운딩 박스를 사용
    let mut min_x = f32::MAX;
    let mut min_y = f32::MAX;
    let mut max_x = f32::MIN;
    let mut max_y = f32::MIN;

    for &[x, y] in points {
        min_x = min_x.min(x);
        min_y = min_y.min(y);
        max_x = max_x.max(x);
        max_y = max_y.max(y);
    }

    let w = max_x - min_x;
    let h = max_y - min_y;

    MinAreaRect {
        points: vec![
            [min_x, min_y],
            [max_x, min_y],
            [max_x, max_y],
            [min_x, max_y],
        ],
        min_side: w.min(h),
    }
}

fn box_score_fast(bitmap: &[f32], bh: usize, bw: usize, box_pts: &[[f32; 2]]) -> f32 {
    let mut xmin = bw as f32;
    let mut xmax = 0f32;
    let mut ymin = bh as f32;
    let mut ymax = 0f32;

    for &[x, y] in box_pts {
        xmin = xmin.min(x);
        xmax = xmax.max(x);
        ymin = ymin.min(y);
        ymax = ymax.max(y);
    }

    let xmin = (xmin.floor() as usize).max(0).min(bw - 1);
    let xmax = (xmax.ceil() as usize).max(0).min(bw - 1);
    let ymin = (ymin.floor() as usize).max(0).min(bh - 1);
    let ymax = (ymax.ceil() as usize).max(0).min(bh - 1);

    if xmax <= xmin || ymax <= ymin {
        return 0.0;
    }

    let mut sum = 0f64;
    let mut count = 0u32;
    for y in ymin..=ymax {
        for x in xmin..=xmax {
            sum += bitmap[y * bw + x] as f64;
            count += 1;
        }
    }

    if count == 0 { 0.0 } else { (sum / count as f64) as f32 }
}

fn unclip(points: &[[f32; 2]], ratio: f32) -> Vec<[f32; 2]> {
    // 간소화된 unclip: 박스를 ratio만큼 확장
    let area = polygon_area(points);
    let perimeter = polygon_perimeter(points);

    if perimeter < 1e-6 {
        return points.to_vec();
    }

    let distance = area * ratio / perimeter;

    // 센터를 기준으로 확장
    let (cx, cy) = polygon_center(points);

    points
        .iter()
        .map(|&[x, y]| {
            let dx = x - cx;
            let dy = y - cy;
            let dist = (dx * dx + dy * dy).sqrt();
            if dist < 1e-6 {
                [x, y]
            } else {
                let scale = (dist + distance) / dist;
                [cx + dx * scale, cy + dy * scale]
            }
        })
        .collect()
}

fn polygon_area(pts: &[[f32; 2]]) -> f32 {
    let n = pts.len();
    if n < 3 {
        return 0.0;
    }
    let mut area = 0f32;
    for i in 0..n {
        let j = (i + 1) % n;
        area += pts[i][0] * pts[j][1];
        area -= pts[j][0] * pts[i][1];
    }
    area.abs() / 2.0
}

fn polygon_perimeter(pts: &[[f32; 2]]) -> f32 {
    let n = pts.len();
    let mut peri = 0f32;
    for i in 0..n {
        let j = (i + 1) % n;
        let dx = pts[j][0] - pts[i][0];
        let dy = pts[j][1] - pts[i][1];
        peri += (dx * dx + dy * dy).sqrt();
    }
    peri
}

fn polygon_center(pts: &[[f32; 2]]) -> (f32, f32) {
    let n = pts.len() as f32;
    let cx = pts.iter().map(|p| p[0]).sum::<f32>() / n;
    let cy = pts.iter().map(|p| p[1]).sum::<f32>() / n;
    (cx, cy)
}

/// 간단한 연결 컴포넌트 탐색 (flood fill)
fn find_contours(bitmap: &[u8], h: usize, w: usize) -> Vec<Vec<[f32; 2]>> {
    let mut visited = vec![false; h * w];
    let mut contours = Vec::new();

    for y in 0..h {
        for x in 0..w {
            let idx = y * w + x;
            if bitmap[idx] == 0 || visited[idx] {
                continue;
            }

            // BFS로 연결 영역 찾기
            let mut component = Vec::new();
            let mut queue = std::collections::VecDeque::new();
            queue.push_back((x, y));
            visited[idx] = true;

            let mut min_x = x;
            let mut max_x = x;
            let mut min_y = y;
            let mut max_y = y;

            while let Some((cx, cy)) = queue.pop_front() {
                component.push((cx, cy));
                min_x = min_x.min(cx);
                max_x = max_x.max(cx);
                min_y = min_y.min(cy);
                max_y = max_y.max(cy);

                for &(dx, dy) in &[(0isize, 1isize), (0, -1), (1, 0), (-1, 0)] {
                    let nx = cx as isize + dx;
                    let ny = cy as isize + dy;
                    if nx >= 0 && nx < w as isize && ny >= 0 && ny < h as isize {
                        let ni = ny as usize * w + nx as usize;
                        if bitmap[ni] == 1 && !visited[ni] {
                            visited[ni] = true;
                            queue.push_back((nx as usize, ny as usize));
                        }
                    }
                }
            }

            if component.len() < 4 {
                continue;
            }

            // 컴포넌트의 경계 박스를 4점으로 반환
            contours.push(vec![
                [min_x as f32, min_y as f32],
                [max_x as f32, min_y as f32],
                [max_x as f32, max_y as f32],
                [min_x as f32, max_y as f32],
            ]);
        }
    }

    contours
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn DB_후처리_텍스트_영역_검출() {
        // 10x10 히트맵에서 (2,2)~(6,6) 영역에 텍스트가 있다고 가정
        let (h, w) = (10, 10);
        let mut pred = vec![0.0f32; h * w];
        for y in 2..=6 {
            for x in 2..=6 {
                pred[y * w + x] = 0.8; // THRESH(0.3) 초과
            }
        }

        let boxes = db_postprocess(&pred, h, w, 100, 100);
        assert!(!boxes.is_empty(), "텍스트 박스가 검출되어야 함");

        // 첫 번째 박스가 올바른 영역에 있는지 확인
        let b = &boxes[0];
        for pt in b {
            assert!(pt[0] >= 0.0 && pt[0] <= 100.0);
            assert!(pt[1] >= 0.0 && pt[1] <= 100.0);
        }
    }

    #[test]
    fn DB_후처리_빈_히트맵() {
        let (h, w) = (10, 10);
        let pred = vec![0.0f32; h * w];
        let boxes = db_postprocess(&pred, h, w, 100, 100);
        assert!(boxes.is_empty(), "빈 히트맵에서 박스가 없어야 함");
    }

    #[test]
    fn 연결_컴포넌트_분리_검출() {
        // 두 개의 분리된 영역
        let (h, w) = (10, 20);
        let mut bitmap = vec![0u8; h * w];
        // 영역 1: (1,1)~(4,4)
        for y in 1..=4 {
            for x in 1..=4 {
                bitmap[y * w + x] = 1;
            }
        }
        // 영역 2: (1,10)~(4,14)
        for y in 1..=4 {
            for x in 10..=14 {
                bitmap[y * w + x] = 1;
            }
        }

        let contours = find_contours(&bitmap, h, w);
        assert_eq!(contours.len(), 2, "두 개의 연결 컴포넌트가 검출되어야 함");
    }
}
