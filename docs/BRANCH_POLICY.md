# Film3DS 分支與合併政策

## 整合目標

目前由 `minmain` 承接所有新功能、修正與分支合併，**不可直接把工作分支合併到 `主要`**。原預設分支 `主要` 暫時保留原狀；GitHub 建立 PR 時若預設選了 `主要`，建立前請主動將 base 改為 `minmain`。此文件是工作約定，並非已啟用的 GitHub branch protection；若要從平台上強制，需另外配置 repository ruleset 與預設分支。

1. 從最新 `minmain` 建立功能／修正分支，完整保留來源提交與文件。
2. 新 PR 設定 **base = `minmain`**、head = 工作分支；不要使用 `主要`。
3. 修改前確認最新 `minmain` 沒有已經包含同一修正；僅匯入仍有價值的檔案，避免舊分支覆蓋新主程式。
4. 每個 PR 至少通過契約、主機測試、RAW／暗房、原生媒體、FilmProbe 故障測試與 ARM11 3DSX（若有相應改動）；影響硬體行為時另外附上人工實測。
5. 只在 CI 綠燈、已檢查二進位／資源授權和使用者資料保護之後合併到 `minmain`。發行到 `主要` 是獨立決策，不自動跟隨。
6. 任何刪除舊分支的決定都要先比較提交、獨有檔案、PR 關聯及保存的硬體證據；本次整合未刪除既有分支。

## LUT 安全與構建

- 預設 ROMFS 不內建任何 `.cube`；已安裝於 SD `/3ds/Film3DS/luts/` 的檔案保留、不覆蓋、不因更新 app 而刪除。
- 兩個分別驗證的 3DSX：`Film3DS.3dsx` 為主程式，`FilmProbe.3dsx` 為獨立手動實驗工具；均從 Actions `Film3DS-ARM11` 產物取得，並提供 SHA-256。
- 主機或交叉編譯成功不等於相機曝光、感光元件 RAW、GPU 與 SD 已在真機測試。
- 專案的完整曝光證據分支 `codex/exposure-handoff-20261112` 仍獨立保留；選取資料或腳本整合前要核對相依版本和內容雜湊。

## 本次收斂紀錄

- `codex/film3ds-foundation` → 已為 `minmain` 提交祖先，不重複合併。
- `codex/raw-darkroom-sync-20261117` → `minmain` 產品程式的基線。
- `ci/3dsx-validation-and-lut-safety-20261118` → PR #4，已合併。
- `docs/mt9v113-reference-catalog-20261115` → 非覆蓋式內容挑選，PR #5，已合併；包含原 camera-hardware-handoff 的有用公開證據和研究索引。
- `codex/camera-hardware-probe` → 獨立 FilmProbe 與 CI，PR #6，已合併。
- `codex/exposure-handoff-code-20261112` → 獨有歷史交接檔，PR #7，已合併。
- `codex/exposure-handoff-20261112` → 完整約 151 MB 實測檔案仍保留原分支，未直接合入 `minmain`。

此表的「已合併」指選擇性匯入有用內容；對於 PR #5～#7，原分支的每一個舊提交並未直接合成為祖先。原始分支在清理之前應保留，避免遺失差異與追溯資訊。
