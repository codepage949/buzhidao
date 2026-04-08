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
            // mean/std는 채널 위치 순서로 적용 (PaddleOCR이 OpenCV BGR에 그대로 적용)
            tensor[[0, y, x]] = (pixel[2] as f32 * SCALE - MEAN[0]) / STD[0]; // B
            tensor[[1, y, x]] = (pixel[1] as f32 * SCALE - MEAN[1]) / STD[1]; // G
            tensor[[2, y, x]] = (pixel[0] as f32 * SCALE - MEAN[2]) / STD[2]; // R
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
        if rect.min_side < MIN_SIZE {
            continue;
        }

        // 박스 점수: min_area_rect의 4점을 polygon mask로 사용 (PaddleOCR 방식)
        let score = box_score_poly(pred, pred_h, pred_w, &rect.points);
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
    let hull = convex_hull(points);
    if hull.len() < 2 {
        return MinAreaRect {
            points: hull.clone(),
            min_side: 0.0,
        };
    }

    // 각 hull edge 방향으로 투영해 최소 면적 회전 사각형을 찾는다
    let mut best_area = f32::MAX;
    let mut best_rect = vec![[0.0f32; 2]; 4];
    let mut best_w = 0.0f32;
    let mut best_h = 0.0f32;

    let n = hull.len();
    for i in 0..n {
        let j = (i + 1) % n;
        let ex = hull[j][0] - hull[i][0];
        let ey = hull[j][1] - hull[i][1];
        let len = (ex * ex + ey * ey).sqrt();
        if len < 1e-6 {
            continue;
        }
        // edge 단위 벡터 + 수직 벡터
        let ux = ex / len;
        let uy = ey / len;
        let vx = -uy;
        let vy = ux;

        // hull 점들을 (u, v) 좌표로 투영
        let mut min_u = f32::MAX;
        let mut max_u = f32::MIN;
        let mut min_v = f32::MAX;
        let mut max_v = f32::MIN;

        for &[px, py] in &hull {
            let u = px * ux + py * uy;
            let v = px * vx + py * vy;
            min_u = min_u.min(u);
            max_u = max_u.max(u);
            min_v = min_v.min(v);
            max_v = max_v.max(v);
        }

        let w = max_u - min_u;
        let h = max_v - min_v;
        let area = w * h;

        if area < best_area {
            best_area = area;
            best_w = w;
            best_h = h;
            // 4개 꼭짓점을 원래 좌표로 역변환
            best_rect = vec![
                [min_u * ux + min_v * vx, min_u * uy + min_v * vy],
                [max_u * ux + min_v * vx, max_u * uy + min_v * vy],
                [max_u * ux + max_v * vx, max_u * uy + max_v * vy],
                [min_u * ux + max_v * vx, min_u * uy + max_v * vy],
            ];
        }
    }

    MinAreaRect {
        points: best_rect,
        min_side: best_w.min(best_h),
    }
}

/// Andrew's monotone chain 알고리즘으로 convex hull을 구한다.
fn convex_hull(points: &[[f32; 2]]) -> Vec<[f32; 2]> {
    let mut pts: Vec<[f32; 2]> = points.to_vec();
    pts.sort_by(|a, b| a[0].partial_cmp(&b[0]).unwrap().then(a[1].partial_cmp(&b[1]).unwrap()));
    pts.dedup();

    let n = pts.len();
    if n <= 2 {
        return pts;
    }

    let mut hull = Vec::with_capacity(2 * n);

    // lower hull
    for &p in &pts {
        while hull.len() >= 2 {
            let a = hull[hull.len() - 2];
            let b = hull[hull.len() - 1];
            if cross(a, b, p) <= 0.0 {
                hull.pop();
            } else {
                break;
            }
        }
        hull.push(p);
    }

    // upper hull
    let lower_len = hull.len() + 1;
    for &p in pts.iter().rev() {
        while hull.len() >= lower_len {
            let a = hull[hull.len() - 2];
            let b = hull[hull.len() - 1];
            if cross(a, b, p) <= 0.0 {
                hull.pop();
            } else {
                break;
            }
        }
        hull.push(p);
    }

    hull.pop(); // 마지막 점은 첫 점과 중복
    hull
}

fn cross(o: [f32; 2], a: [f32; 2], b: [f32; 2]) -> f32 {
    (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
}

/// 폴리곤 마스크 기반 박스 점수 (PaddleOCR box_score_fast 방식).
/// box_pts의 4점을 폴리곤으로 채워서 내부 픽셀만 평균한다.
fn box_score_poly(pred: &[f32], bh: usize, bw: usize, box_pts: &[[f32; 2]]) -> f32 {
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

    let xmin = (xmin.floor() as usize).max(0).min(bw.saturating_sub(1));
    let xmax = (xmax.ceil() as usize).max(0).min(bw.saturating_sub(1));
    let ymin = (ymin.floor() as usize).max(0).min(bh.saturating_sub(1));
    let ymax = (ymax.ceil() as usize).max(0).min(bh.saturating_sub(1));

    if xmax <= xmin || ymax <= ymin {
        return 0.0;
    }

    let mut sum = 0f64;
    let mut count = 0u32;
    for y in ymin..=ymax {
        for x in xmin..=xmax {
            if point_in_polygon(x as f32, y as f32, box_pts) {
                sum += pred[y * bw + x] as f64;
                count += 1;
            }
        }
    }

    if count == 0 { 0.0 } else { (sum / count as f64) as f32 }
}

/// 점이 폴리곤 내부에 있는지 판단 (ray casting).
fn point_in_polygon(px: f32, py: f32, polygon: &[[f32; 2]]) -> bool {
    let n = polygon.len();
    let mut inside = false;
    let mut j = n - 1;
    for i in 0..n {
        let (xi, yi) = (polygon[i][0], polygon[i][1]);
        let (xj, yj) = (polygon[j][0], polygon[j][1]);
        if ((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi) {
            inside = !inside;
        }
        j = i;
    }
    inside
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

/// 외곽선 추적 (OpenCV findContours RETR_LIST 방식).
/// Suzuki-Abe border following 알고리즘의 간소화 버전.
fn find_contours(bitmap: &[u8], h: usize, w: usize) -> Vec<Vec<[f32; 2]>> {
    // 1px 패딩 추가 (경계 처리 단순화)
    let ph = h + 2;
    let pw = w + 2;
    let mut padded = vec![0i32; ph * pw];
    for y in 0..h {
        for x in 0..w {
            if bitmap[y * w + x] != 0 {
                padded[(y + 1) * pw + (x + 1)] = 1;
            }
        }
    }

    let mut contours = Vec::new();
    let mut nbd: i32 = 1; // border 번호

    // 8방향 이웃 (시계 방향): E, SE, S, SW, W, NW, N, NE
    let dir8: [(isize, isize); 8] = [
        (1, 0),
        (1, 1),
        (0, 1),
        (-1, 1),
        (-1, 0),
        (-1, -1),
        (0, -1),
        (1, -1),
    ];

    for y in 1..ph - 1 {
        for x in 1..pw - 1 {
            let idx = y * pw + x;

            // 외곽선 시작점: 0→1 전이 (외부 경계)
            if padded[idx] == 1 && padded[idx - 1] == 0 {
                nbd += 1;
                let contour = trace_border(&mut padded, pw, x, y, nbd, 0, &dir8);
                if contour.len() >= 4 {
                    // 패딩 보정: (-1, -1)
                    let pts: Vec<[f32; 2]> = contour
                        .iter()
                        .map(|&(cx, cy)| [(cx as isize - 1) as f32, (cy as isize - 1) as f32])
                        .collect();
                    contours.push(pts);
                }
            }
            // 내부 경계: 1→0 전이
            else if padded[idx] >= 1 && padded[idx + 1] == 0 {
                nbd += 1;
                trace_border(&mut padded, pw, x, y, nbd, 4, &dir8);
                // 내부 경계(홀)는 무시 (RETR_LIST에서는 반환하지만 OCR에선 불필요)
            }

            // 이미 방문된 외곽선 내부 픽셀 스킵 방지
            if padded[idx] != 0 && padded[idx] == 1 {
                padded[idx] = -nbd;
            }
        }
    }

    contours
}

/// 하나의 외곽선을 추적한다 (border following).
fn trace_border(
    img: &mut [i32],
    w: usize,
    start_x: usize,
    start_y: usize,
    nbd: i32,
    start_dir: usize,
    dir8: &[(isize, isize); 8],
) -> Vec<(usize, usize)> {
    let mut contour = Vec::new();

    // 시작 방향에서 반시계 방향으로 첫 번째 이웃 찾기
    let first_neighbor = find_first_nonzero_neighbor(img, w, start_x, start_y, start_dir, dir8);

    let (first_x, first_y, first_dir) = match first_neighbor {
        Some(v) => v,
        None => {
            // 고립 픽셀
            img[start_y * w + start_x] = -nbd;
            return vec![(start_x, start_y)];
        }
    };

    contour.push((start_x, start_y));

    let mut cx = first_x;
    let mut cy = first_y;
    let mut from_dir = (first_dir + 4) % 8; // 돌아온 방향

    loop {
        contour.push((cx, cy));

        // 이 픽셀의 경계 마킹
        let idx = cy * w + cx;
        if img[idx] == 1 {
            img[idx] = nbd;
        } else if img[idx] > 1 {
            // 이미 다른 경계에 속함 — 그대로 유지
        }

        // 다음 이웃 찾기 (from_dir에서 시계 방향으로)
        let search_start = (from_dir + 2) % 8; // from_dir의 다음 시계 방향
        let next = find_first_nonzero_neighbor(img, w, cx, cy, search_start, dir8);

        match next {
            Some((nx, ny, nd)) => {
                from_dir = (nd + 4) % 8;
                cx = nx;
                cy = ny;
            }
            None => break,
        }

        if cx == first_x && cy == first_y && contour.len() > 1 {
            // 시작점으로 돌아옴 — 중복 제거
            break;
        }

        if contour.len() > 100_000 {
            break; // 안전 장치
        }
    }

    contour
}

/// 주어진 방향부터 시계 방향으로 순회하며 첫 번째 비-제로 이웃을 찾는다.
fn find_first_nonzero_neighbor(
    img: &[i32],
    w: usize,
    x: usize,
    y: usize,
    start_dir: usize,
    dir8: &[(isize, isize); 8],
) -> Option<(usize, usize, usize)> {
    for i in 0..8 {
        let d = (start_dir + i) % 8;
        let (dx, dy) = dir8[d];
        let nx = x as isize + dx;
        let ny = y as isize + dy;
        if nx >= 0 && ny >= 0 {
            let nx = nx as usize;
            let ny = ny as usize;
            let idx = ny * w + nx;
            if idx < img.len() && img[idx] != 0 {
                return Some((nx, ny, d));
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{DynamicImage, Rgb, RgbImage};

    #[test]
    fn BGR_정규화가_채널_위치_순서로_적용된다() {
        let mut img = RgbImage::new(1, 1);
        img.put_pixel(0, 0, Rgb([10, 20, 30]));

        let tensor = preprocess(&DynamicImage::ImageRgb8(img));

        let expected_b = (30.0 * SCALE - MEAN[0]) / STD[0];
        let expected_g = (20.0 * SCALE - MEAN[1]) / STD[1];
        let expected_r = (10.0 * SCALE - MEAN[2]) / STD[2];

        assert!((tensor[[0, 0, 0, 0]] - expected_b).abs() < 1e-6);
        assert!((tensor[[0, 1, 0, 0]] - expected_g).abs() < 1e-6);
        assert!((tensor[[0, 2, 0, 0]] - expected_r).abs() < 1e-6);
    }

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

    #[test]
    fn 폴리곤_점수는_배경을_제외한다() {
        let (h, w) = (6, 6);
        let mut pred = vec![0.1f32; h * w];
        let polygon = [[2.0, 1.0], [4.0, 2.0], [3.0, 4.0], [1.0, 3.0]];

        for y in 0..h {
            for x in 0..w {
                if point_in_polygon(x as f32, y as f32, &polygon) {
                    pred[y * w + x] = 0.9;
                }
            }
        }

        let score = box_score_poly(&pred, h, w, &polygon);
        assert!(score > 0.85, "폴리곤 내부 평균 점수가 유지되어야 함: {score}");
    }
}
