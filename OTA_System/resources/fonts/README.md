# fonts/

여기에 `NotoSansCJK-Regular.ttc`를 추가하세요.

- 출처: [Google Noto Fonts](https://fonts.google.com/noto) 공식 배포본
  (Noto Sans CJK, Regular 굵기, `.ttc` 형식)
- 용도: 라즈베리파이(욕토 이미지)엔 한글(CJK) 폰트가 없을 수 있어서,
  `OTA_System` 실행 파일과 함께 이 폰트를 배포해 화면의 한글 라벨/로그가
  깨지지 않게 함. 로드 코드는 `main.cpp`의 `loadKoreanFont()`.
- 빌드 연동: `CMakeLists.txt`가 이 파일이 있으면 자동으로 실행 파일 옆
  (build 디렉터리와 install 디렉터리 둘 다)으로 복사함. 없으면 CMake
  설정 단계에서 경고만 뜨고 빌드는 됨(한글만 깨져 보임).

이 폴더 자체는 커밋되지만, 실제 폰트 파일(수십 MB)은 별도로 받아서
로컬에 넣어야 합니다.
