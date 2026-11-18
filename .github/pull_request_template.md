<!-- Film3DS integration policy: https://github.com/film-64/Film3DS/blob/minmain/docs/BRANCH_POLICY.md -->
<!-- IMPORTANT: choose minmain as the GitHub PR base. The repository's default branch may still be 主要. -->

## 變更目的與來源分支

## 驗證

- [ ] PR base 選的是 `minmain`（非 `主要`）。
- [ ] 已檢查目前 `minmain` 的程式和提交，未把新版代碼覆蓋成舊版。
- [ ] 契約、主機、RAW／暗房、原生媒體、探針及 ARM11 CI 適用項目通過。
- [ ] 確認 `Film3DS.3dsx` / `FilmProbe.3dsx` 相關產物與 SHA-256。
- [ ] 修改涉及 SD 檔案時，確認原有的 `/3ds/Film3DS/luts/*.cube` 不會刪除或覆蓋。
- [ ] 涉及真實相機／硬體時，清楚區分可重複的主機測試與實機測量，附上實機證據或說明未測。
- [ ] 若引用第三方程式、LUT、素材或硬體紀錄，核對授權與來源。

## 風險、回退與尚未驗證項目
