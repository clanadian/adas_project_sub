// RoiProposer 단위 테스트 뼈대.
//
// ROI 후보 생성 알고리즘을 확정한 후 추가할 테스트:
// 1. 잘못된 입력 프레임 처리
// 2. 후보가 없는 프레임
// 3. bbox 좌표와 objectness 범위
// 4. 중복 bbox 제거
// 5. objectness 정렬과 max_candidates=10 제한
// 6. frame_id와 roi_id 할당
