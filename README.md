# Even G2 → Muse / M5Stack StopWatch

Even AI の Agent Configuration から、Cloudflare を通して StopWatch 上の
Muse Gadget に発言を送り、Muse の確定した返答を G2 に返すブリッジです。

```text
Even G2 ⇄ Evenアプリ
              ⇅ HTTPS /v1/chat/completions
       Cloudflare Worker + Durable Object
              ⇅ WSS /device/ws（StopWatchから接続）
       M5Stack StopWatch / Muse Gadget
              ⇅ 公式SDKの認証済みNoiseセッション
             Muse
```

Even AI の公式 BYOA 接続を使います。Terminal モードは使いません。
SDKトークンはアカウントのチャットAPIキーではありません。端末をMuseアプリで
ペアリングして初めて会話できます。Museの認証情報は端末に置き、Cloudflareには渡しません。

画面中央に公式Museキャラクターのアニメーション、下部に Cloudflare / Muse の接続状態を表示します。
公開SDKに残量APIがないため、トークン残量の表示には対応していません。

## 構成

- `cloudflare/`: HTTPS受付、専用端末とのWebSocket、OpenAI Chat Completions互換応答。
- `firmware/`: 固定版の公式Muse SDKへ適用する追加ソースとパッチ。
- `scripts/prepare_firmware.py`: 固定コミットを取得し `.build/sdk` に組み立てます。
- `scripts/configure.py`: 秘密値を `.private/` に作成します。Git対象外、アクセス権0600。
- `scripts/idf.sh`: StopWatch向けESP-IDFビルド、menuconfig、明示指定したポートへの書き込み。
- `upstream.json`: SDKとESP-IDFの固定コミット。

元のSDKは `e6fe5174092702ffddae6f90759a38d55a4ebae1`、ESP-IDFは **v6.0.1**。
Arduino用の既存ビルド環境ではなく、公式Muse SDKが要求するESP-IDFを使用します。
標準のBLEペアリング、Wi-Fi設定、画面・電源ドライバを利用します。
StopWatch自身のマイク入力・スピーカー再生は無効です。G2側で音声認識し、
端末の内部RAMは接続処理へ回します。青ボタンのペアリング承認は利用できます。
カスタムブリッジが標準ファームウェアで上書きされないよう、SDKのOTA更新は無効です。
待受中もG2から到達できるようWi-Fi接続を保持します。画面は通常どおり消灯しますが、
標準SDKのWi-Fi休止時より待機電力が増えます。電池持ちはまだ実測していません。

## 1. Cloudflare Workerを作成する

[Cloudflare Workersの作成・設定手順](docs/cloudflare-setup.md) に、初回設定をまとめています。
アカウント準備 → Wranglerログイン → WorkerとDurable Objectの初回公開 →
取得したURLで端末設定・認証キーを生成 → キー登録 → 動作確認、の順に進めてください。
Worker名の既定は `even-muse-bridge`。独自ドメインやCloudflare Tunnelは不要です。
アカウント・会話ごとの設定は `cloudflare/wrangler.local.toml` に保存し、
Git管理対象の `wrangler.toml` は公開用テンプレートとして使用します。

Muse SDKトークンが未取得でもCloudflare側は先に設定できます。
利用するCloudflareアカウントで、作成手順に従って自分用のWorkerをデプロイしてください。

## 2. MuseとEvenの接続情報を設定する

Cloudflareの手順中で `scripts/configure.py` を実行すると、Even用と端末用の認証キー、
公開URLに対応する端末設定が `.private/` に作成されます。
Muse SDKトークンは [Muse Gadgets](https://gadgets.muse.ai/) の Account → SDK tokens で取得し、
同じスクリプトの非表示入力欄から登録します。後から再実行しても既存の認証キーは保持されます。

Evenアプリの **設定 → Even AI → Agent Configuration** に、
`.private/even-settings.txt` の Endpoint と API Key を設定します。
端末用の `DEVICE_TOKEN` やMuseのSDKトークンはEvenに設定しません。
詳細は [作成手順の手順5〜8](docs/cloudflare-setup.md#5-端末設定と認証キーを生成する)、
受付と端末の通信仕様は [Cloudflare README](cloudflare/README.md) を参照してください。

## 3. ファームウェアをビルド

初回はPython 3.10以上とCMakeを用意し、プロジェクトルートから次を実行します。
ESP-IDFと関連ツールは、Git管理対象外の `.tools/` に導入します。
macOSではApple付属Pythonが古い場合があるため、使用するPythonのPATHを先に設定してください。

```sh
git clone --depth 1 --branch v6.0.1 --recurse-submodules --shallow-submodules \
  https://github.com/espressif/esp-idf.git .tools/esp-idf
IDF_TOOLS_PATH="$PWD/.tools/espressif" .tools/esp-idf/install.sh esp32s3
```

```sh
python3 scripts/prepare_firmware.py
scripts/idf.sh build
```

生成物は `.build/stopwatch/muse-gadget.bin`。SDKトークンやWorker接続情報が未設定でも
コンパイル確認はできますが、実際の会話はできません。設定後に再ビルドしてください。
`.private/`、ローカル接続設定、ビルド生成物、フラッシュのバックアップはGit管理対象外です。
実機用バイナリには接続先や秘密値が含まれるため、リリース添付などで配布しないでください。
組み立て済みの `.build/sdk` は生成物なので、編集は `firmware/` の追加ソース・パッチへ行います。
`prepare_firmware.py` は生成済みSDKを作り直すため、ビルド中に実行しないでください。

## 4. 書き込みとペアリング

書き込みは現在の端末ファームウェアを置き換えます。対象がStopWatchであることを確認し、
既存ファームウェア・設定を戻す必要がある場合は16MBフラッシュ全体のバックアップを先に保存します。
USBの `303a:1001` という識別だけではESP32-S3の機種までは確定できません。

対象ポートを確認してから実行します（次はポート名の例です）。

```sh
scripts/idf.sh -p /dev/cu.usbmodemYOUR-DEVICE flash
scripts/idf.sh -p /dev/cu.usbmodemYOUR-DEVICE monitor
```

Museアプリの Settings → Devices でDeveloper modeを有効にし、
`MuseGadget` で始まるStopWatchを追加します。端末で承認を求められたら、
USB端子を下にして右上の青ボタンを押します。Wi-Fi設定とペアリングを完了し、
画面で Cloudflare online / Muse ready を確認してからEven AIへ話しかけます。
初期ペアリング中はCloudflareブリッジを起動せず、初期設定用の内部RAMを確保します。
ペアリング完了後に自動再起動し、保存済み設定で接続します。
G2での会話の確認範囲は [検証記録](docs/verification.md) を参照してください。

## 会話の扱いと限界

- 1台・1アカウント用です。同時に処理する発言は1件。処理中の追加要求は409で返します。
- Museの専用セッションが履歴を保持し、Evenからは最後のuser発言だけを送ります。
  過去履歴、system/developerメッセージ、クライアント指定modelはMuseへ再送しません。
- 入力は8KiB、受付JSONは32KiB、返答全体は64KiBまで。画像・音声データの直接入力は未対応です。
  G2の音声認識はEvenアプリ側に任せます。
- Museの確定回答を集めてから返します。SSE形式にも対応しますが、逐次生成中の文字は流しません。
- 待機上限は既定120秒。これはEvenアプリの保証値ではなく、実機で調整する設定です。
- 期限切れ・切断時は再送しません。中断通知はMuse側ですでに実行された操作の取消を保証しません。
- Museの別会話を混ぜないため、専用session_id、送信ACKのmessage_id、返信parentを確認します。
- 公開SDKにはAPI安定性の保証がないため、サーバー側変更で動作が変わる可能性があります。

## 検証

```sh
cd cloudflare
npm test
node --test tools/runtime-check.mjs
```

2つ目は実際のCloudflare `workerd` をローカル起動して確認します。
ファームウェアのホストテストは `firmware/tests/`、ビルドログは `.build/idf-build.log`。
現時点の検証結果は [検証記録](docs/verification.md) を参照してください。

## 公式資料

- [Even AI BYOA設定](https://support.evenrealities.com/hc/en-us/articles/16280926743695-Tutorial-Bridging-G2-to-OpenClaw-Bring-Your-Own-Agent)
- [Meta Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk)
- [M5Stack StopWatch](https://docs.m5stack.com/en/core/StopWatch)
- [Cloudflare Durable Objects WebSocket](https://developers.cloudflare.com/durable-objects/best-practices/websockets/)

SDKのライセンスはApache-2.0（同梱の第三者コードには各ライセンスが適用）。
本プロジェクトのパッチは元ファイルの著作権表示を維持します。
ライセンス本文と変更対象は [第三者コードの表示](THIRD_PARTY_NOTICES.md) を参照してください。
