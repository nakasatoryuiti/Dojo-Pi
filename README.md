# 土壌水分センサー ＆ LINE通知システム

MCP3008 A/DコンバータとRaspberry Piを使い、土壌水分センサーの値を監視し、
状態変化(水を感知/乾燥を検知)をLINE Messaging APIでスマートフォンに通知します。

## 旧版からの変更点

| 項目 | 旧版 | 現版 |
|---|---|---|
| SPI通信 | sysfs GPIOの手動ビットバンギング | カーネルSPIドライバ経由(`/dev/spidev0.0`) |
| GPIO制御 | sysfs (`/sys/class/gpio`、非推奨API) | `libgpiod`(センサー電源制御のみに使用) |
| ネットワーク呼び出し | タイムアウト無し | 接続5秒/全体10秒でタイムアウト |
| 送信失敗時 | 何もしない(通知消失) | 指数バックオフで3回リトライ、結果をログに記録 |
| 認証情報 | ソースコードに直書き | 環境変数から読み込み |
| 閾値の根拠 | コメント無し | 本READMEにキャリブレーション手順を明記 |
| 終了処理 | 無限ループで到達不能 | SIGINT/SIGTERMで安全に終了 |

## 必要なパッケージ(Raspberry Pi OS / Debian系)

```bash
sudo apt update
sudo apt install -y build-essential libcurl4-openssl-dev libgpiod-dev
```

## SPIの有効化

`raspi-config` の `Interface Options > SPI` で有効化するか、
`/boot/firmware/config.txt` に以下を追記して再起動してください。

```
dtparam=spi=on
```

有効化後、`/dev/spidev0.0` が存在することを確認してください。

```bash
ls -l /dev/spidev0.0
```

## GPIOチップ名の確認(Pi4 / Pi5 差異の注意)

旧版では固定のsysfs GPIO番号(520〜523)を使用しており、これは
Raspberry Pi 4のBCM番号とは整合しない、実機不明な番号でした。
本版ではspidevがSPI通信自体を吸収するため、この問題自体が解消されています。
残る唯一のGPIO(センサー電源制御用)についても、番号ではなく
「gpiochip名 + BCMライン番号」で指定するため、実機に応じて明示的に確認してください。

```bash
gpioinfo
```

実行結果に表示されるチップ名を確認し、環境変数 `GPIO_CHIP` で指定します。

- Raspberry Pi 4: 通常 `gpiochip0`
- Raspberry Pi 5 (RP1): 通常 `gpiochip4` (OS/カーネルバージョンにより変動するため必ず`gpioinfo`で確認)

## 資料

- `回路図.pdf` — MCP3008・センサー・Raspberry Piの接続回路図
- `データのフロー図.pdf` — センサー値取得からLINE通知までのデータフロー図
- `ラズパイの配線写真.jpg` — 実機の配線写真

GitHub上では、これらをREADME内に画像として埋め込むと、実際に組み立てて検証したことが伝わりやすくなります(例: `![配線写真](./ラズパイの配線写真.jpg)`)。

## 配線

| MCP3008 | 接続先 |
|---|---|
| CLK | SPI0 SCLK (GPIO11) |
| DOUT | SPI0 MISO (GPIO9) |
| DIN | SPI0 MOSI (GPIO10) |
| CS/SHDN | SPI0 CE0 (GPIO8) |
| VDD, VREF | 3.3V |
| AGND, DGND | GND |
| CH1 | 土壌水分センサー AOUT(実機検証の結果、CH0は接触不良のため使用せずCH1を採用) |

土壌水分センサーのVCCは、直接3.3V/5Vへ常時接続するのではなく、
本プログラムが制御する電源ライン(デフォルト: BCM17、環境変数`SENSOR_POWER_LINE`で変更可)
を経由して接続してください。GPIOの出力電流(Raspberry Piは1ピンあたり最大16mA程度)を
超える可能性があるセンサーの場合は、GPIOで直接駆動せず、トランジスタ/MOSFETで
スイッチングしてください。

## 設定手順(環境変数)

トークン等をソースコードに直書きしないでください。`.env.example` をコピーして使用します。

```bash
cp .env.example .env
# .env を編集し、実際の値を設定してください
export $(grep -v '^#' .env | xargs)
```

`.env` に設定する項目:

- `LINE_TOKEN` : LINE Messaging APIのチャネルアクセストークン(必須)
- `LINE_USER_ID` : 通知先のLINEユーザーID(必須)
- `SPI_DEVICE` : SPIデバイスパス(省略時 `/dev/spidev0.0`)
- `SPI_SPEED_HZ` : SPIクロック速度(省略時 1350000)
- `GPIO_CHIP` : gpiochip名(省略時 `gpiochip0`。Pi5等では要変更、上記参照)
- `SENSOR_POWER_LINE` : センサー電源制御に使うBCMライン番号(省略時 17)
- `MOISTURE_THRESHOLD` : 水分判定の閾値(省略時 600。下記キャリブレーション参照)
- `LOG_FILE_PATH` : ログファイルの保存先(省略時 `./soil_sensor.log`)

**`.env` は絶対にGitにコミットしないでください**(`.gitignore` に追加済み)。

## ビルド

```bash
make
```

## 実行

```bash
export $(grep -v '^#' .env | xargs)
./soil_monitor
```

`Ctrl+C` または `kill` で安全に終了します(SIGINT/SIGTERMをハンドリングし、
SPI/GPIOのクローズ、ログファイルのフラッシュを行った上で終了します)。

## 閾値キャリブレーション(重要)

`MOISTURE_THRESHOLD`(デフォルト600)は、使用するセンサーの個体差・電源電圧・
土壌の種類によって最適値が変わります。以下の手順で必ず実機キャリブレーションを
行ってください。

```bash
export $(grep -v '^#' .env | xargs)
./soil_monitor --calibrate
```

1. センサー先端を完全に乾燥させた状態(空気中)で実行し、安定したADC値を
   5〜10個メモする → これを `dry_value` とする
2. センサー先端を水、または十分に湿らせた土壌に挿した状態で実行し、
   同様に安定したADC値をメモする → これを `wet_value` とする
3. `dry_value` と `wet_value` の大小関係と、使用しているセンサーの仕様
   (値が小さいほど湿っているタイプか、大きいほど湿っているタイプか)を確認する
4. 中間値付近(例: `(dry_value + wet_value) / 2`)を目安に `MOISTURE_THRESHOLD` を
   `.env` に設定し、実際の判定境界が意図通りに動作するか再度動作確認する

`Main.c` の判定ロジックは以下の通りです(センサー値が小さいほど湿潤と判定するタイプを想定)。
自分のセンサーの特性に合わせて `Main.c` の比較演算子を調整してください。

```c
int current_status = (value < g_threshold && value > 10) ? 1 : 0; // 1: 水分検知
```

`value > 10` は、センサー未接続・断線時に値が0付近に張り付くケースを
誤検知しないための下限フィルタです。

## ログ

すべての状態変化・送信結果・エラーは `LOG_FILE_PATH`(デフォルト
`./soil_sensor.log`)に、タイムスタンプ付きで追記されます。LINE送信が
3回リトライしても失敗した場合は `ERROR` レベルで記録され、次に状態が
変化するタイミングで再送が試みられます。

## systemdサービスとして常駐させる場合(参考)

```ini
# /etc/systemd/system/soil-monitor.service
[Unit]
Description=Soil Moisture Sensor LINE Notifier
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/home/pi/soil_monitor
EnvironmentFile=/home/pi/soil_monitor/.env
ExecStart=/home/pi/soil_monitor/soil_monitor
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now soil-monitor
journalctl -u soil-monitor -f
```

## 既知の制約・今後の改善余地

- リトライは同一プロセス内での即時再送のみで、キューイングや永続化は行っていない
- 複数センサー・複数チャンネル同時監視には未対応(CH1固定)
- ヒステリシス(閾値付近でのチャタリング対策)は未実装。ノイズが大きい環境では
  上限/下限2つの閾値を設けるヒステリシス方式への拡張を推奨
