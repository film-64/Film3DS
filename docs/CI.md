# Film3DS CI 與 3DSX 產物

CI 的唯一正式 Homebrew 產物是 `Film3DS.3dsx`。所有 push、PR、手動觸發都執行完整驗證；不依檔案名稱跳過測試，避免相機／暗房／儲存的跨模組回歸。CI 預設不執行實機測試，也不聲稱測得真實快門、ISO、感光元件 RAW 或相機韌體行為。

| 工作 | 驗證範圍 | 主要指令 |
| --- | --- | --- |
| `contracts` | Python 語法、相機契約、可選的研究參考索引、未內建私有 CUBE | `python3 -m compileall -q tools tests`、`camera_runtime_contract.py --check`、可選 `camera_reference.py --verify` |
| `host` | C 核心、儲存/DNG、LUT 掃描與安裝、一般整合測試、ASan/UBSan | `make -j2 test`、`make sanitize` |
| `raw-darkroom` | RAW、Roll、GPU 模擬、暗房預覽、顯示校準、輸出 | `make -j2 test-raw-darkroom` |
| `native-interop` | 原生媒體解碼、相簿檔案檢查與互通、拒絕不可信輸入 | `make test-native-integration` |
| `arm11` | devkitARM 3DSX、ARM 指令限制、3DSX 格式與打包、SHA-256 | `make -j2 3ds`、`verify_arm11.sh`、`verify_3dsx.py` |

`arm11` 使用固定 digest 的官方 devkitPro 容器，並保留標量 libjpeg-turbo 授權聲明。成功時於 GitHub Actions 執行頁面的 Artifacts 區取得 `Film3DS-ARM11`，包含 `Film3DS.3dsx`、`Film3DS.3dsx.sha256`、授權檔。正式合併前應確認所有工作成功，並避免使用 `[skip ci]` 導致未經驗證的主程式提交。

## .cube 與使用者資料安全

預設 `romfs/` 不打包 `.cube`。程式啟動時掃描 SD 卡 `/3ds/Film3DS/luts/` 內所有一般檔案、大小寫不敏感的 `.cube`；既有檔不刪除、不覆蓋。若未來選擇手動隨包帶入 LUT，僅複製 SD 上缺少的同名檔。當 ROMFS 沒有 `luts/` 資料夾時，安裝程序應當成功略過，隨後仍掃描 SD；這個情境由 `tests/standard_media.c` 自動回歸測試。

CI 對 `romfs/` 中任何 `.cube` 給出錯誤，確保此分支的「無內建 LUT」預設沒有被意外改變。如將來要提供可選的使用者自備 LUT 打包，需明確調整此政策與測試，不可用更新程式來清理既有 SD `.cube`。

## 分支整合前提（2026-11-18）

目前 `主要` 仍是初始提交；以 `codex/raw-darkroom-sync-20261117` 作為產品整合基礎。先把此 CI 工作分支合併至該基礎並確認結果，再視需要把經測試的整合分支 PR 合入 `主要`。

- `codex/film3ds-foundation` 是其他工作的共同祖先，PR #1 尚未合併；移動預設分支／PR base 前不得刪除。
- `codex/camera-hardware-probe` 的 PR #2 提供獨立 FilmProbe，與 RAW 分支有共同檔案修改；要挑選並移植獨立探針與測試，勿直接盲目覆蓋。
- `docs/mt9v113-reference-catalog-20261115` 的 PR #3 以 `codex/camera-hardware-handoff-20261114` 為 base，已包含後者全部提交；PR #3 重新定基、驗證和整合後，舊 camera-handoff 才可刪。
- `codex/exposure-handoff-code-20261112` 的程式檔基本上與較完整的 exposure-handoff 相同，但仍有獨有 `CODE_FIRST_STATUS.txt`，歸檔後可刪。
- `codex/exposure-handoff-20261112` 保存大量實測資料（約 151 MB）；不要直接併進日常產品分支，以免產物歷史膨脹。保存記錄並擇需抽取工具或測試。

測試通過僅證明主機/模擬與 ARM11 打包約束；3DS 實機相機、SD 耐久、曝光控制與畫面正確性仍須另行測量。
