# 検証記録 — 2026-10-23

## Museキャラクターの表示

- 公式SDKに残っていたキャラクターのアニメーションを表示し、Cloudflare / Museの接続状態と
  Even G2の待受表示を画面下部の2行へ移動しました。端末の音声機能は無効のままです。
- ESP-IDFビルド成功後、アプリ領域だけを書き込み、書き込みデータのハッシュ一致を確認しました。
  バイナリは2,166,784 bytesです。実機用バイナリには接続先や秘密値が含まれます。
  バイナリ・生ログ・画面画像は公開物に含めません。
- 保存済み設定でCloudflareのWebSocketとMuseの会話セッションへ再接続。
  待受時の内部RAMは約33KB、最大連続空き23KBでした。
- 公式SDKのUSBスナップショット機能を有効にし、実機の466×466画面を取得しました。
  キャラクターとステータスの重なり・文字切れがないことを画像で確認しました。
  取得画像は `.build/muse-avatar.png`（確認用に3倍拡大）、ビルドログは `.build/idf-build-avatar.log` です。
- この表示更新後、端末ログで発言の受信から13.8秒後の返信完了も確認しました。
  下記のG2上での表示確認は、この表示更新より前の通信修正版に対する実機操作者の確認です。

## 実機での修正・接続確認

M5Stack StopWatch（ESP32-S3、8MB PSRAM）へアプリ領域だけを書き込み、
保存済みWi-Fi・ペアリング情報を保持して確認しました。

- 初期ペアリング中のブリッジ起動を停止。修正前は最大連続空き7KBに対して
  初期設定タスクが8KBを要求し、`error_operation_in_progress` で切断していました。
- 修正後、iPhoneのMuseアプリから `provision_v2` → `wifi_connected` → `auth_ok` →
  ペアリング完了・再起動をシリアルログで確認しました。
- 端末自身の音声入力・再生を無効化。codecの電源を切り、
  I2S/DMA・音声タスクを起動しません。ブリッジのタスクスタックと返信ID配列はPSRAMへ移しました。
- 音声無効版で `muse_vm` の生成・資格情報取得、MuseのNoiseセッション、
  `/chat/subscribe` の接続に成功。待受時の内部RAMは約31〜34KB、最大連続空き22〜23KBです。
  修正前の接続失敗時は約14KB、最大4KBでした。
- Cloudflareの再接続拒否も修正して公開。認証済みの新接続で旧接続を置き換え、
  旧会話は明示エラーで終了します。実機の `Cloudflare websocket connected` を確認しました。
- 修正に対するホスト確認：Link契約74件、会話処理16件、返信バッファ1件成功。
  Cloudflare単体16件、workerd統合1件成功。
- 初回専用チャットの404に限り、最初の発言でチャットを作成し、ACKを検証してから購読するよう修正。
  2回目以降は購読完了を確認してから送信します。
- 本番Worker → USB接続中のStopWatch → Muse → Workerの実通信で、
  「接続テストです。『接続成功』とだけ返してください。」に対して「接続成功」が返りました。
  初回JSON応答はHTTP 200、11.75秒。次のSSE応答もHTTP 200、8.47秒で `[DONE]` まで完了しました。
- 更新後の再起動でWi-Fi・Muse・Cloudflareへ自動再接続しました。ペアリングの再実施は不要でした。
  バイナリサイズ・テスト件数・実機での確認結果は [hardware-verification.json](hardware-verification.json) に記録しています。

初回の非常に速い返答は購読前に終わる可能性があり、その場合はタイムアウトで終了します。
自動再送は行いません。今回の専用チャットは作成・往復確認済みです。
G2からの発言についても端末ログで受信・約26.5秒後の返信完了を確認し、
実機操作者が **G2にも返答が表示されたこと** を確認しました。
Wi-Fi切断からの復帰、長時間待受・電池持ちは未確認です。

## 初期ビルド時の確認（実機修正前）

- **ESP-IDF v6.0.1 / StopWatch実ターゲットのビルド・リンク・容量チェック**。
  最終ソースを、通信設定が空の状態と、通信処理を含む検証用設定の両方でビルド。
  最終バイナリは2,166,784 bytes、4MiBアプリ領域の空き48%。
- 公式SDKのホストテスト **632件、630成功、2スキップ**。
  今回追加したMuseセッション分離、ACK前の保留、確定回答の置換、取消後の遅延返信除外を含みます。
- 返信バッファのホストテスト **1件成功**。ASan/UBSan付きで、最大長、複数メッセージ、
  古い世代、空の確定回答、UTF-8境界を確認。
- CloudflareのNodeテスト **14件成功**。
- Cloudflareの実行エンジンworkerdを使うローカル統合テスト **1件成功**。
  認証済みWebSocket、日本語・絵文字のJSON/SSE往復、別IDの排除、同時要求、
  タイムアウト、キャンセル、端末切断を確認。
- Wrangler **4.152.0** の `deploy --dry-run` 成功。公開操作は行っていません。
- Python/Bash/JavaScript構文チェック、固定SDKへのパッチ適用チェック成功。

## スキップした2件

- `test_noise_core.NoiseCoreCompileTest.test_imported_core_compiles_and_links_with_psa_crypto`:
  ホスト側のPSA Crypto開発ヘッダー・ライブラリ未導入。
- `test_ppk2_profile.BenchmarkTest.test_synthetic_benchmark_counts_every_sample_without_hardware`:
  電力測定用 `ppk2-api==0.9.2` 未導入。

ESP32向けの暗号ライブラリはESP-IDFビルドに含まれます。
ホストテストでは既存のローカルHTTPテストからソケットのResourceWarningが出ましたが、失敗はありません。

## 初期ビルド時点で未実施だったもの

- Cloudflareへの公開：Wranglerは未ログインでした。
- Muse SDKトークンの設定と、MuseアプリでのStopWatchペアリング。
- USB書き込み、画面の実機表示、Even G2からMuseまでの実通信。
- Evenアプリの実リクエスト形式・待機時間、Wi-Fi再接続、長時間待受、電池持ちの実測。

USBのEspressif Serial/JTAG機器が1台見えましたが、機種を断定せず、リセット・書き込みはしていません。
スモークテストでは実際のMuseアカウントやユーザー会話を使用していません。

初期ビルド時のバイナリは **検証用URL `compile-check.invalid` とダミーの端末キー**を
含むビルド確認用でした。現在のバイナリには実際の接続設定が含まれるため、配布しないでください。
`scripts/configure.py` で本物の接続情報を設定し、再ビルドしてから書き込んでください。
`scripts/idf.sh` は検証用URLが残った状態のflashを拒否します。

初期ビルドの固定SDK・サイズ・SHA-256は [build-verification.json](build-verification.json) に記録しています。
これらの確認は、実機で会話が成立したことを意味しません。

## 再現コマンド

プロジェクトルートから:

```sh
python3 scripts/prepare_firmware.py
scripts/idf.sh build
python3 -m unittest discover -s firmware/tests -p 'test_*.py'
export IDF_TOOLS_PATH="$PWD/.tools/espressif"
. .tools/esp-idf/export.sh
python3 -m unittest discover -s .build/sdk/esp32/tests -p 'test_*.py'
```

`export.sh` で、その環境に導入したESP-IDF用Pythonを有効にします。
SDKテスト中のHTTP待受はローカルポートを使います。

```sh
cd cloudflare
npm test
npm run test:runtime
npx wrangler deploy --dry-run
```
