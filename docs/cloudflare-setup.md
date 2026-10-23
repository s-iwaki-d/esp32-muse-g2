# Cloudflare Workersの作成・設定手順

このプロジェクトの `cloudflare/` にあるコードを使い、Even G2とStopWatchを中継するWorkerを作ります。
以下はmacOSのターミナルで進める初回設定です。MuseのSDKトークンがまだなくても、Cloudflare側は先に設定できます。

## 1. Cloudflareアカウントを準備する

1. [Cloudflareダッシュボード](https://dash.cloudflare.com/)にログインします。アカウントがなければ作成します。
2. 使用するアカウントの **Workers & Pages** を開きます。
3. `workers.dev` のサブドメインを確認します。初回利用で登録を求められた場合は、任意の名前を登録します。CLIで初回公開するときに案内された場合も、その案内に従って登録できます。

公開URLは `https://Worker名.サブドメイン.workers.dev` になります。
たとえばWorker名が `even-muse-bridge`、サブドメインが `your-subdomain` なら、
`https://even-muse-bridge.your-subdomain.workers.dev` です。
サブドメインはCloudflareのアカウントIDやメールアドレスとは別の値です。
独自ドメインを購入・登録する必要はありません。
([公式：workers.dev](https://developers.cloudflare.com/workers/configuration/routing/workers-dev/))

この構成では、StopWatchがCloudflareへ接続します。Cloudflare Tunnelの作成や自宅ルーターのポート開放は不要です。

## 2. Wranglerを準備してログインする

Node.js 22以上とPython 3を用意します。Node.jsが未導入なら、[公式ダウンロード](https://nodejs.org/en/download)からLTS版を導入してください。
このリポジトリのルートから実行します。

```sh
cd cloudflare
node --version
python3 --version
npm ci --ignore-scripts
test -f wrangler.local.toml || cp wrangler.toml wrangler.local.toml
npx wrangler login
npx wrangler whoami
```

`wrangler login` で開くブラウザで、使用するCloudflareアカウントへのアクセスを許可します。
`whoami` にログイン状態とアカウントが表示されれば準備完了です。
Wranglerはこのプロジェクトの固定バージョンを使用します。新しいWorkerプロジェクトを別途作る必要はありません。
`wrangler.toml` は公開用テンプレートです。上のコマンドは自分用の `wrangler.local.toml` が
ない場合だけコピーします。以降のアカウント・会話設定はGit管理対象外のローカルファイルへ書き込みます。

複数のアカウントがある場合は、`whoami` に表示された対象のAccount IDを
`cloudflare/wrangler.local.toml` の先頭側、**`[vars]` より上**に追加します。

```toml
account_id = "対象アカウントのAccount ID"
```

## 3. Worker名と会話IDを設定する

`cloudflare/wrangler.local.toml` をエディタで開きます。
公開用テンプレートの [`wrangler.toml`](../cloudflare/wrangler.toml) は変更しません。

- `name = "even-muse-bridge"` がWorker名です。このまま利用できます。同じアカウントに別用途の同名Workerがある場合は、作成前に名前を変更してください。
- `MUSE_SESSION_ID` はG2専用のMuse会話IDです。初回に次のコマンドでUUIDを1つ作り、空欄に設定します。

```sh
uuidgen
```

```toml
[vars]
REQUEST_TIMEOUT_MS = "120000"
MUSE_SESSION_ID = "ここに生成したUUID"
```

同じ会話を継続する間、UUIDは保持します。再デプロイのたびに作り直さないでください。
新しいUUIDの会話は最初の発言で作成されます。初回は短い接続テストを送り、
返答を確認してから使い始めてください。公開SDKには過去の返信を再取得する手段がなく、
初回だけ作成直後の非常に速い返答を取り逃す可能性があります。タイムアウトしても
自動再送しません。Museアプリで届いたかを確認してから次の発言を送ります。
`main`、`durable_objects.bindings`、`migrations` は設定済みなので、そのまま使用します。
MuseのSDKトークンや認証キーは、このファイルには書き込みません。

## 4. Workerを初回作成・公開する

`cloudflare/` ディレクトリで実行します。`npm run deploy` は `wrangler.local.toml` を使用し、実際にCloudflare上のリソースを作成・公開します。

```sh
npm run deploy
```

既存のコードと設定から、次のものが作成されます。

| 作成・設定されるもの | このプロジェクトの名前 |
| --- | --- |
| HTTPSリクエストを受けるWorker | `even-muse-bridge`（手順3で変更可能） |
| 接続中のStopWatchと会話要求を管理するDurable Objectのクラス | `MuseBridge` |
| WorkerからDurable Objectを呼び出すバインディング | `BRIDGE` |

Durable Objectのクラス登録は設定ファイルのSQLite用マイグレーションに含まれています。
ダッシュボードでDurable Objectやデータベースを手作業で追加する必要はありません。
([公式：Durable Objectsのデプロイ](https://developers.cloudflare.com/durable-objects/get-started/))

成功するとターミナルに `https://even-muse-bridge.実際のサブドメイン.workers.dev` のようなURLが表示されます。
**以降は、その実際のURLを使用してください。** ダッシュボードでは **Workers & Pages → 作成したWorker** から確認できます。

この段階では認証キーが未登録なので、会話と端末接続は `503 / not_configured` で拒否されます。次の手順でキーを登録します。

## 5. 端末設定と認証キーを生成する

プロジェクトのルートに戻り、次のURLを手順4で表示されたものに置き換えて実行します。
ここでは `/v1/chat/completions` などのパスを付けません。

```sh
cd ..
python3 scripts/configure.py --worker-url https://even-muse-bridge.YOUR-SUBDOMAIN.workers.dev
```

Muse SDK tokenの入力を求められます。取得済みなら入力します。文字は画面に表示されません。
**未取得ならEnterで空欄のまま進められます。** 後から同じコマンドを再実行して追加できます。
トークンは [Muse Gadgets](https://gadgets.muse.ai/) の Account → SDK tokens で取得します。

スクリプトが以下を作成します。すべてGit管理対象外です。

| ファイル | 内容・用途 |
| --- | --- |
| `.private/cloudflare-secrets.json` | Even用 `EVEN_API_KEY` と端末用 `DEVICE_TOKEN`。Cloudflareへ登録する2つのキー |
| `.private/even-settings.txt` | Evenアプリに入力するEndpointとAPI Key |
| `.private/sdkconfig.local` | StopWatch用のWSS URL・端末キー・入力済みのMuse SDKトークン |
| `cloudflare/.dev.vars` | ローカルでWorkerを試すときのキー |

既存のキーは再実行しても保持されます。Muse SDKトークンはCloudflare用JSONには含まれません。

## 6. Cloudflareに認証キーを登録する

```sh
cd cloudflare
npx wrangler secret bulk ../.private/cloudflare-secrets.json --config wrangler.local.toml
npx wrangler secret list --config wrangler.local.toml
```

`secret bulk` が2つのキーを登録し、Workerに反映します。`secret list` では値を表示せず、
`EVEN_API_KEY` と `DEVICE_TOKEN` が登録されていることを確認できます。
キーの値をコマンドに直接書く必要はありません。
([公式：Wranglerのsecretコマンド](https://developers.cloudflare.com/workers/wrangler/commands/workers/#secret-bulk))

ダッシュボードでも **Worker → Settings → Variables and Secrets** で名前を確認できます。
この手順ではCLIから一括登録するため、ダッシュボードで重ねて入力する必要はありません。
([公式：Secrets](https://developers.cloudflare.com/workers/configuration/secrets/))

## 7. 公開状態を確認する

以下もURLを実際のものに置き換えます。

```sh
curl -i https://even-muse-bridge.YOUR-SUBDOMAIN.workers.dev/health
```

HTTP `200` と次のJSONが返ればWorkerが起動しています。

```json
{"ok":true,"service":"even-muse-bridge"}
```

次は、認証キーなしの会話要求を拒否することの確認です。この要求はMuseへ送られません。

```sh
curl -i -X POST \
  https://even-muse-bridge.YOUR-SUBDOMAIN.workers.dev/v1/chat/completions \
  -H 'Content-Type: application/json' \
  --data '{"messages":[{"role":"user","content":"接続確認"}]}'
```

HTTP `401`、エラーコード `unauthorized` が期待値です。
`503 / not_configured` なら手順6のキー登録先を確認します。
`/health` の成功とこの認証チェックはWorker側の確認です。StopWatchやMuseとの接続完了は、次の手順で確認します。

## 8. EvenアプリとStopWatchをつなぐ

`.private/even-settings.txt` をローカルのテキストエディタで開き、
Evenアプリの **設定 → Even AI → Agent Configuration** に以下を入力します。

| Evenの設定欄 | 入力する値 |
| --- | --- |
| Endpoint | ファイルの `Endpoint:` の値。末尾は `/v1/chat/completions` |
| API Key | ファイルの `API Key:` の値。Cloudflareの `EVEN_API_KEY` と同じ |

Muse SDKトークンや `DEVICE_TOKEN` はEvenアプリに入力しません。
StopWatchが使う `wss://…/device/ws` と `DEVICE_TOKEN` は手順5で端末設定に保存済みです。

Muse SDKトークンが未取得なら、取得後に手順5を再実行します。
その後、[READMEのファームウェアビルド・書き込み・ペアリング手順](../README.md#3-ファームウェアをビルド)に進みます。
画面で **Cloudflare online / Muse ready** を確認できたら、G2のEven AIから話しかけて返答を確認します。

## 更新するとき

Workerのコードだけを更新した場合は、同じアカウント・同じWorker名のまま再デプロイします。
登録済みのsecretは保持されます。次はリポジトリのルートから実行します。

```sh
cd cloudflare
npm run deploy
```

WorkerのURLを変更した場合は、手順5を新URLで再実行し、EvenのEndpointも更新します。
StopWatchは設定後に再ビルド・書き込みが必要です。別のWorkerを作った場合は、手順6のキー登録も行います。

## うまくつながらない場合

| 状態 | 確認するところ |
| --- | --- |
| Wranglerが未認証、または別アカウントを表示する | `npx wrangler login`、`whoami` と `account_id` を確認 |
| `workers.dev` の登録を求められる | 表示された案内に従ってサブドメインを登録し、再度デプロイ |
| ブラウザのログイン画面やHTMLが返る | このAPIのURLにCloudflare Accessのブラウザ認証が掛かっていないか確認。EvenとStopWatchはこのブリッジのBearerキーを使用 |
| URLの `/` を開くと404 | 正常。動作確認は `/health`、EvenのEndpointは `/v1/chat/completions` |
| `503 / not_configured` | エラーメッセージを確認。キー未登録なら手順6、会話UUID不正なら手順3を修正して再デプロイ |
| キーを設定したEvenから `401 / unauthorized` | Evenのキーが `.private/even-settings.txt` のAPI Keyと同じか確認 |
| `503 / device_offline` | StopWatchが未接続。電源・Wi-Fi・端末側URLと `DEVICE_TOKEN` を確認 |
| `409 / device_busy` | 前の会話処理が完了するまで待つ |
| `502 / muse_unavailable` | Museのペアリング・SDKトークン・Muse側の接続状態を確認 |
| `504 / timeout` | Museが返答するまでに上限を超過。Muse側の状態を確認 |

実機での確認範囲は [検証記録](verification.md) を参照してください。自分のアカウントと機器での動作確認は、この手順に沿って行います。
