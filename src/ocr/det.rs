use image::DynamicImage;
use ndarray::{Array3, Array4};
use ort::session::Session;

/// DB 후처리 파라미터 (inference.yml 기반)
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
fn resize_for_det(img: &DynamicImage, resize_long: u32) -> (DynamicImage, f64, f64) {
    let (w, h) = (img.width(), img.height());
    let ratio = if h > w {
        resize_long as f64 / h as f64
    } else {
        resize_long as f64 / w as f64
    };

    let mut new_h = (h as f64 * ratio) as u32;
    let mut new_w = (w as f64 * ratio) as u32;

    let stride = 128u32;
    new_h = ((new_h + stride - 1) / stride) * stride;
    new_w = ((new_w + stride - 1) / stride) * stride;

    let resized = img.resize_exact(new_w, new_h, image::imageops::FilterType::Triangle);
    let ratio_h = new_h as f64 / h as f64;
    let ratio_w = new_w as f64 / w as f64;

    (resized, ratio_h, ratio_w)
}

/// BGR 순서로 정규화 + CHW 텐서 변환
fn preprocess(img: &DynamicImage) -> Array4<f32> {
    let rgb = img.to_rgb8();
    let (w, h) = (rgb.width() as usize, rgb.height() as usize);

    // as_raw()로 연속 메모리 직접 접근: bounds check 없이 RGBRGB... 순서로 순회
    let raw = rgb.as_raw();
    let mut tensor = Array3::<f32>::zeros((3, h, w));
    for (i, chunk) in raw.chunks_exact(3).enumerate() {
        let y = i / w;
        let x = i % w;
        // BGR 순서 (PaddleOCR det는 BGR 입력)
        // mean/std는 채널 위치 순서로 적용 (PaddleOCR이 OpenCV BGR에 그대로 적용)
        tensor[[0, y, x]] = (chunk[2] as f32 * SCALE - MEAN[0]) / STD[0]; // B
        tensor[[1, y, x]] = (chunk[1] as f32 * SCALE - MEAN[1]) / STD[1]; // G
        tensor[[2, y, x]] = (chunk[0] as f32 * SCALE - MEAN[2]) / STD[2]; // R
    }

    tensor.insert_axis(ndarray::Axis(0))
}

/// DB 후처리: 히트맵에서 텍스트 박스를 추출한다.
fn db_postprocess(
    pred: &[f32],
    pred_h: usize,
    pred_w: usize,
    src_h: u32,
    src_w: u32,
    det_thresh: f32,
    box_thresh: f32,
) -> Result<Vec<DetBox>, String> {
    // 이진화
    let bitmap: Vec<u8> = pred
        .iter()
        .map(|&v| if v > det_thresh { 1u8 } else { 0u8 })
        .collect();

    // OpenCV와 동일한 contour 추출 경로 사용
    let contours = find_contours(&bitmap, pred_h, pred_w)?;

    let w_scale = src_w as f64 / pred_w as f64;
    let h_scale = src_h as f64 / pred_h as f64;

    let mut boxes = Vec::new();
    for contour in contours.iter().take(MAX_CANDIDATES) {
        if contour.len() < 4 {
            continue;
        }

        // 최소 경계 사각형
        let rect = min_area_rect(contour)?;
        if rect.min_side < MIN_SIZE {
            continue;
        }

        // 박스 점수: min_area_rect의 4점을 polygon mask로 사용 (PaddleOCR 방식)
        let score = box_score_poly(pred, pred_h, pred_w, &rect.points);
        if score < box_thresh {
            continue;
        }

        // unclip
        let expanded = unclip(&rect.points, UNCLIP_RATIO)?;
        let expanded_rect = min_area_rect(&expanded)?;
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

    Ok(boxes)
}

pub(crate) fn detect_with_resize_long(
    session: &mut Session,
    img: &DynamicImage,
    resize_long: u32,
    det_thresh: f32,
    box_thresh: f32,
) -> Result<Vec<DetBox>, String> {
    let (src_w, src_h) = (img.width(), img.height());
    let (resized, _ratio_h, _ratio_w) = resize_for_det(img, resize_long);
    let input = preprocess(&resized);

    let pred_h = resized.height() as usize;
    let pred_w = resized.width() as usize;

    let input_values = ort::value::Value::from_array(input).map_err(|e| e.to_string())?;
    let outputs = session
        .run(ort::inputs![input_values])
        .map_err(|e| e.to_string())?;

    let (_shape, data) = outputs[0]
        .try_extract_tensor::<f32>()
        .map_err(|e| format!("det 출력 추출 실패: {e}"))?;
    let pred: Vec<f32> = data.to_vec();

    // 출력은 [1, 1, H, W] 형태
    db_postprocess(&pred, pred_h, pred_w, src_h, src_w, det_thresh, box_thresh)
}

#[cfg(test)]
/// 겹치는 박스를 AABB IoU 기준으로 제거한다 (greedy NMS).
fn deduplicate_boxes(boxes: Vec<DetBox>) -> Vec<DetBox> {
    let n = boxes.len();
    let mut suppress = vec![false; n];

    for i in 0..n {
        if suppress[i] {
            continue;
        }
        for j in i + 1..n {
            if suppress[j] {
                continue;
            }
            if aabb_iou(&boxes[i], &boxes[j]) > 0.5
                || aabb_overlap_ratio_of_smaller(&boxes[i], &boxes[j]) > 0.9
            {
                suppress[j] = true;
            }
        }
    }

    boxes
        .into_iter()
        .zip(suppress)
        .filter(|(_, s)| !s)
        .map(|(b, _)| b)
        .collect()
}

#[cfg(test)]
/// 두 DetBox의 축-정렬 bounding box IoU를 계산한다.
fn aabb_iou(a: &DetBox, b: &DetBox) -> f64 {
    let (ax0, ay0, ax1, ay1) = aabb_bounds(a);
    let (bx0, by0, bx1, by1) = aabb_bounds(b);
    let inter = aabb_intersection_area((ax0, ay0, ax1, ay1), (bx0, by0, bx1, by1));
    if inter <= 0.0 {
        return 0.0;
    }
    let area_a = (ax1 - ax0) * (ay1 - ay0);
    let area_b = (bx1 - bx0) * (by1 - by0);
    let union = area_a + area_b - inter;

    if union <= 0.0 {
        0.0
    } else {
        inter / union
    }
}

#[cfg(test)]
fn aabb_overlap_ratio_of_smaller(a: &DetBox, b: &DetBox) -> f64 {
    let (ax0, ay0, ax1, ay1) = aabb_bounds(a);
    let (bx0, by0, bx1, by1) = aabb_bounds(b);
    let inter = aabb_intersection_area((ax0, ay0, ax1, ay1), (bx0, by0, bx1, by1));
    if inter <= 0.0 {
        return 0.0;
    }
    let area_a = (ax1 - ax0) * (ay1 - ay0);
    let area_b = (bx1 - bx0) * (by1 - by0);
    let smaller = area_a.min(area_b);
    if smaller <= 0.0 {
        0.0
    } else {
        inter / smaller
    }
}

#[cfg(test)]
fn aabb_bounds(b: &DetBox) -> (f64, f64, f64, f64) {
    let x0 = b.iter().map(|p| p[0]).fold(f64::MAX, f64::min);
    let x1 = b.iter().map(|p| p[0]).fold(f64::MIN, f64::max);
    let y0 = b.iter().map(|p| p[1]).fold(f64::MAX, f64::min);
    let y1 = b.iter().map(|p| p[1]).fold(f64::MIN, f64::max);
    (x0, y0, x1, y1)
}

#[cfg(test)]
fn aabb_intersection_area(a: (f64, f64, f64, f64), b: (f64, f64, f64, f64)) -> f64 {
    let ix0 = a.0.max(b.0);
    let ix1 = a.2.min(b.2);
    let iy0 = a.1.max(b.1);
    let iy1 = a.3.min(b.3);
    if ix1 <= ix0 || iy1 <= iy0 {
        0.0
    } else {
        (ix1 - ix0) * (iy1 - iy0)
    }
}

// ── 기하 유틸리티 ────────────────────────────────────────────────────────────

struct MinAreaRect {
    points: Vec<[f32; 2]>,
    min_side: f32,
}

fn min_area_rect(points: &[[f32; 2]]) -> Result<MinAreaRect, String> {
    let hull = convex_hull(points);
    if hull.len() < 2 {
        return Ok(MinAreaRect {
            points: hull.clone(),
            min_side: 0.0,
        });
    }

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

        let ux = ex / len;
        let uy = ey / len;
        let vx = -uy;
        let vy = ux;

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
            best_rect = vec![
                [min_u * ux + min_v * vx, min_u * uy + min_v * vy],
                [max_u * ux + min_v * vx, max_u * uy + min_v * vy],
                [max_u * ux + max_v * vx, max_u * uy + max_v * vy],
                [min_u * ux + max_v * vx, min_u * uy + max_v * vy],
            ];
        }
    }

    Ok(MinAreaRect {
        points: order_box_points(&best_rect),
        min_side: best_w.min(best_h),
    })
}

fn order_box_points(points: &[[f32; 2]]) -> Vec<[f32; 2]> {
    if points.len() != 4 {
        return points.to_vec();
    }

    let mut pts = points.to_vec();
    pts.sort_by(|a, b| a[0].partial_cmp(&b[0]).unwrap());

    let (left0, left1) = if pts[0][1] <= pts[1][1] {
        (pts[0], pts[1])
    } else {
        (pts[1], pts[0])
    };

    let (right0, right1) = if pts[2][1] <= pts[3][1] {
        (pts[2], pts[3])
    } else {
        (pts[3], pts[2])
    };

    vec![left0, right0, right1, left1]
}

/// Andrew's monotone chain 알고리즘으로 convex hull을 구한다.
fn convex_hull(points: &[[f32; 2]]) -> Vec<[f32; 2]> {
    let mut pts: Vec<[f32; 2]> = points.to_vec();
    pts.sort_by(|a, b| {
        a[0].partial_cmp(&b[0])
            .unwrap()
            .then(a[1].partial_cmp(&b[1]).unwrap())
    });
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

    if count == 0 {
        0.0
    } else {
        (sum / count as f64) as f32
    }
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

fn unclip(points: &[[f32; 2]], ratio: f32) -> Result<Vec<[f32; 2]>, String> {
    let area = polygon_area(points);
    let perimeter = polygon_perimeter(points);
    if perimeter < 1e-6 || points.len() < 3 {
        return Ok(points.to_vec());
    }

    let distance = area * ratio / perimeter;
    Ok(offset_convex_polygon(points, distance))
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

fn offset_convex_polygon(points: &[[f32; 2]], distance: f32) -> Vec<[f32; 2]> {
    let n = points.len();
    let signed_area = signed_polygon_area(points);
    let orientation = if signed_area >= 0.0 { 1.0 } else { -1.0 };
    let mut shifted = Vec::with_capacity(n);

    for i in 0..n {
        let p0 = points[i];
        let p1 = points[(i + 1) % n];
        let dx = p1[0] - p0[0];
        let dy = p1[1] - p0[1];
        let len = (dx * dx + dy * dy).sqrt();
        if len < 1e-6 {
            continue;
        }

        let outward = if orientation > 0.0 {
            [dy / len, -dx / len]
        } else {
            [-dy / len, dx / len]
        };
        shifted.push([
            [p0[0] + outward[0] * distance, p0[1] + outward[1] * distance],
            [p1[0] + outward[0] * distance, p1[1] + outward[1] * distance],
        ]);
    }

    if shifted.len() < 3 {
        return points.to_vec();
    }

    let mut expanded = Vec::with_capacity(shifted.len());
    for i in 0..shifted.len() {
        let prev = shifted[(i + shifted.len() - 1) % shifted.len()];
        let curr = shifted[i];
        expanded.push(line_intersection(prev[0], prev[1], curr[0], curr[1]).unwrap_or(curr[0]));
    }
    expanded
}

fn line_intersection(a1: [f32; 2], a2: [f32; 2], b1: [f32; 2], b2: [f32; 2]) -> Option<[f32; 2]> {
    let x1 = a1[0];
    let y1 = a1[1];
    let x2 = a2[0];
    let y2 = a2[1];
    let x3 = b1[0];
    let y3 = b1[1];
    let x4 = b2[0];
    let y4 = b2[1];

    let denom = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4);
    if denom.abs() < 1e-6 {
        return None;
    }

    let det_a = x1 * y2 - y1 * x2;
    let det_b = x3 * y4 - y3 * x4;
    Some([
        (det_a * (x3 - x4) - (x1 - x2) * det_b) / denom,
        (det_a * (y3 - y4) - (y1 - y2) * det_b) / denom,
    ])
}

fn signed_polygon_area(pts: &[[f32; 2]]) -> f32 {
    let n = pts.len();
    if n < 3 {
        return 0.0;
    }
    let mut area = 0f32;
    for i in 0..n {
        let j = (i + 1) % n;
        area += pts[i][0] * pts[j][1] - pts[j][0] * pts[i][1];
    }
    area / 2.0
}

fn find_contours(bitmap: &[u8], h: usize, w: usize) -> Result<Vec<Vec<[f32; 2]>>, String> {
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
    let mut nbd: i32 = 1;
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

            if padded[idx] == 1 && padded[idx - 1] == 0 {
                nbd += 1;
                let contour = trace_border(&mut padded, pw, x, y, nbd, 0, &dir8);
                if contour.len() >= 4 {
                    let pts: Vec<[f32; 2]> = contour
                        .iter()
                        .map(|&(cx, cy)| [(cx as isize - 1) as f32, (cy as isize - 1) as f32])
                        .collect();
                    contours.push(pts);
                }
            } else if padded[idx] >= 1 && padded[idx + 1] == 0 {
                nbd += 1;
                trace_border(&mut padded, pw, x, y, nbd, 4, &dir8);
            }

            if padded[idx] != 0 && padded[idx] == 1 {
                padded[idx] = -nbd;
            }
        }
    }

    Ok(contours)
}

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
    let first_neighbor = find_first_nonzero_neighbor(img, w, start_x, start_y, start_dir, dir8);

    let (first_x, first_y, first_dir) = match first_neighbor {
        Some(v) => v,
        None => {
            img[start_y * w + start_x] = -nbd;
            return vec![(start_x, start_y)];
        }
    };

    contour.push((start_x, start_y));

    let mut cx = first_x;
    let mut cy = first_y;
    let mut from_dir = (first_dir + 4) % 8;

    loop {
        contour.push((cx, cy));

        let idx = cy * w + cx;
        if img[idx] == 1 {
            img[idx] = nbd;
        }

        let search_start = (from_dir + 2) % 8;
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
            break;
        }

        if contour.len() > 100_000 {
            break;
        }
    }

    contour
}

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
    fn bgr_정규화가_채널_위치_순서로_적용된다() {
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
    fn db_후처리_텍스트_영역_검출() {
        // 10x10 히트맵에서 (2,2)~(6,6) 영역에 텍스트가 있다고 가정
        let (h, w) = (10, 10);
        let mut pred = vec![0.0f32; h * w];
        for y in 2..=6 {
            for x in 2..=6 {
                pred[y * w + x] = 0.8; // THRESH(0.3) 초과
            }
        }

        let boxes =
            db_postprocess(&pred, h, w, 100, 100, 0.3, 0.5).expect("DB 후처리가 성공해야 함");
        assert!(!boxes.is_empty(), "텍스트 박스가 검출되어야 함");

        // 첫 번째 박스가 올바른 영역에 있는지 확인
        let b = &boxes[0];
        for pt in b {
            assert!(pt[0] >= 0.0 && pt[0] <= 100.0);
            assert!(pt[1] >= 0.0 && pt[1] <= 100.0);
        }
    }

    #[test]
    fn db_후처리_빈_히트맵() {
        let (h, w) = (10, 10);
        let pred = vec![0.0f32; h * w];
        let boxes =
            db_postprocess(&pred, h, w, 100, 100, 0.3, 0.5).expect("DB 후처리가 성공해야 함");
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

        let contours = find_contours(&bitmap, h, w).expect("contour 추출이 성공해야 함");
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
        assert!(
            score > 0.85,
            "폴리곤 내부 평균 점수가 유지되어야 함: {score}"
        );
    }

    #[test]
    fn unclip은_중심_스케일링이_아니라_에지_오프셋으로_확장한다() {
        let rect = [[0.0, 0.0], [10.0, 0.0], [10.0, 2.0], [0.0, 2.0]];
        let expanded = unclip(&rect, 1.5).expect("unclip이 성공해야 함");

        let min_x = expanded.iter().map(|p| p[0]).fold(f32::MAX, f32::min);
        let max_x = expanded.iter().map(|p| p[0]).fold(f32::MIN, f32::max);
        let min_y = expanded.iter().map(|p| p[1]).fold(f32::MAX, f32::min);
        let max_y = expanded.iter().map(|p| p[1]).fold(f32::MIN, f32::max);

        assert!(min_x < 0.0 && max_x > 10.0);
        assert!(min_y < 0.0 && max_y > 2.0);
    }

    #[test]
    fn mini_box_점순서는_좌상_우상_우하_좌하다() {
        let ordered = order_box_points(&[[9.0, 5.0], [1.0, 7.0], [8.0, 1.0], [2.0, 2.0]]);
        assert_eq!(
            ordered,
            vec![[2.0, 2.0], [8.0, 1.0], [9.0, 5.0], [1.0, 7.0]]
        );
    }

    #[test]
    fn det_resize_long은_128_배수로_올림된다() {
        let img = DynamicImage::ImageRgb8(RgbImage::new(1000, 500));
        let (resized, _, _) = resize_for_det(&img, 1024);

        assert_eq!(resized.width(), 1024);
        assert_eq!(resized.height(), 512);
    }

    #[test]
    fn det_resize_long은_작은_입력도_stride에_맞춰_확대될_수_있다() {
        let img = DynamicImage::ImageRgb8(RgbImage::new(854, 480));
        let (resized, ratio_h, ratio_w) = resize_for_det(&img, 1152);

        assert_eq!(resized.width(), 1152);
        assert_eq!(resized.height(), 768);
        assert!(ratio_w > 1.0);
        assert!(ratio_h > 1.0);
    }

    #[test]
    fn aabb_iou_완전_겹침은_1_0() {
        let a: DetBox = [[0.0, 0.0], [10.0, 0.0], [10.0, 5.0], [0.0, 5.0]];
        assert!((aabb_iou(&a, &a) - 1.0).abs() < 1e-9);
    }

    #[test]
    fn aabb_iou_완전_분리는_0_0() {
        let a: DetBox = [[0.0, 0.0], [5.0, 0.0], [5.0, 5.0], [0.0, 5.0]];
        let b: DetBox = [[10.0, 0.0], [15.0, 0.0], [15.0, 5.0], [10.0, 5.0]];
        assert_eq!(aabb_iou(&a, &b), 0.0);
    }

    #[test]
    fn deduplicate_boxes_중복_박스_제거() {
        let a: DetBox = [[0.0, 0.0], [10.0, 0.0], [10.0, 5.0], [0.0, 5.0]];
        let b: DetBox = [[0.5, 0.0], [10.5, 0.0], [10.5, 5.0], [0.5, 5.0]]; // 거의 동일
        let c: DetBox = [[100.0, 0.0], [110.0, 0.0], [110.0, 5.0], [100.0, 5.0]]; // 분리됨
        let result = deduplicate_boxes(vec![a, b, c]);
        assert_eq!(result.len(), 2, "겹치는 박스 하나는 제거되어야 함");
    }

    #[test]
    fn deduplicate_boxes_포함_중복_박스_제거() {
        let large: DetBox = [[0.0, 0.0], [100.0, 0.0], [100.0, 20.0], [0.0, 20.0]];
        let small: DetBox = [[10.0, 2.0], [70.0, 2.0], [70.0, 18.0], [10.0, 18.0]];
        let result = deduplicate_boxes(vec![large, small]);
        assert_eq!(result.len(), 1, "큰 박스 안의 부분 중복 박스는 제거되어야 함");
    }
}
