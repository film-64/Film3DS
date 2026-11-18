# Film3DS CI 與 3DSX 產物

CI 的主程式 Homebrew 產物是 `Film3DS.3dsx`；另提供獨立實驗工具 `FilmProbe.3dsx`，實機使用需明確啟動。所有 push、PR、手動觸發都執行完整驗證；不依檔案名稱跳過測試，避免相機／暗房／儲存的跨模組回歸。CI 預設不執行實機測試，也不聲稱測得真實快門、ISO、感光元件 RAW 或相機韌體行為。

| 工作 | 驗證範圍 | 主要指令 |
| --- | --- | --- |
| `contracts` | Python 語法、相機契約、可選的研究參考索引、未內建私有 CUBE | `python3 -m compileall -q tools tests`、`camera_runtime_contract.py --check`、可選 `camera_reference.py --verify` |
| `host` | C 核心、儲存/DNG、LUT 掃描與安裝、一般整合測試、ASan/UBSan | `make -j2 test`、`make sanitize` |
| `raw-darkroom` | RAW、Roll、GPU 模擬、暗房預覽、顯示校準、輸出 | `make -j2 test-raw-darkroom` |
| `native-interop` | 原生媒體解碼、相簿檔案檢查與互通、拒絕不可信輸入 | `make test-native-integration` |
| `probe-host` | 獨立 FilmProbe 的寫入／還原／中止故障安全測試及報告 | `tests/probe_driver.c`、`tests/exposure_report.py` |
| `arm11` | devkitARM、主程式與獨立探針 3DSX、ARM 指令／格式檢查、SHA-256 | `make -j2 3ds`、`make -j2 -f Makefile.probe`、`verify_arm11.sh`、`verify_3dsx.py` |

`arm11` 使用固定 digest 的官方 devkitPro 容器，並保留標量 libjpeg-turbo 授權聲明。成功時於 GitHub Actions 執行頁面的 Artifacts 區取得 `Film3DS-ARM11`，包含 `Film3DS.3dsx`、`FilmProbe.3dsx`、各自的 SHA-256，以及主程式 JPEG 授權檔。正式合併前應確認所有工作成功，並避免使用 `[skip ci]` 導致未經驗證的主程式提交。

## .cube 與使用者資料安全

預設 `romfs/` 不打包 `.cube`。程式啟動時掃描 SD 卡 `/3ds/Film3DS/luts/` 內所有一般檔案、大小寫不敏感的 `.cube`；既有檔不刪除、不覆蓋。若未來選擇手動隨包帶入 LUT，僅複製 SD 上缺少的同名檔。當 ROMFS 沒有 `luts/` 資料夾時，安裝程序應當成功略過，隨後仍掃描 SD；這個情境由 `tests/standard_media.c` 自動回歸測試。

CI 對 `romfs/` 中任何 `.cube` 給出錯誤，確保此分支的「無內建 LUT」預設沒有被意外改變。如將來要提供可選的使用者自備 LUT 打包，需明確調整此政策與測試，不可用更新程式來清理既有 SD `.cube`。

## minmain 整合政策（2026-11-18）

**`minmain` 是現階段唯一的功能整合目標。** 所有後續功能分支和修正 PR 應以 `minmain` 作為 base，完成相應 CI 後才合併。原預設分支 `主要` 尚未切換，GitHub 建立 PR 時可能自動選中 `主要`，必須手動改選 `minmain`。只有後續明確核准的發行整合才應從 `minmain` 進入 `主要`。詳細步驟見 [分支政策](BRANCH_POLICY.md)。

本次整合已以 `codex/raw-darkroom-sync-20261117` 的最新程式碼為基礎，並合併：

- PR #4：3DSX CI、契約／RAW／原生媒體驗證、SD LUT 安全回歸修正。
- PR #5：MT9V113 相機研究參考索引、公開硬體資料、獨立診斷檔案；未覆蓋最新主程式原有檔案。
- PR #6：獨立 FilmProbe、故障還原測試和第二個 3DSX 產物。
- PR #7：曝光交接中 47 個未與新版檔案重疊的歷史程式與記錄。

`codex/film3ds-foundation` 的提交是 `minmain` 祖先，無須再次合併。較舊分支的 README、Makefile、相機 runtime 等與新版重疊檔案不應無條件覆蓋。完整曝光實測原件仍保存在 `codex/exposure-handoff-20261112`，目前不把約 151 MB 歷史資料直接納入產品分支。原分支及舊 PR 暫不刪除，以保留審查和追溯能力。

CI 測試成功僅證明主機／模擬與 ARM11 打包約束；3DS 實機相機、SD 耐久、曝光控制、畫面正確性仍需實機測量。
