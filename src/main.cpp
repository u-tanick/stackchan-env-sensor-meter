#include <M5Unified.h>
#include <Avatar.h> // https://github.com/meganetaaan/m5stack-avatar
#include "esp32-hal-ledc.h"

// ----------------------------------------------------------------
// アバター関連
using namespace m5avatar;
Avatar avatar;
ColorPalette *cp;

// ----------------------------------------------------------------
// サーボハット関連
uint8_t timer_width = 16;
uint8_t ch = 1;
uint8_t pin = 26;
uint8_t pwm_hz = 50;

// 角度に対応するデューティーサイクルを計算
void setServoAngle(int angle) {
  // 角度に対応するパルス幅を計算
  int minDuty = 3277;  // 約1ms（0°）
  int maxDuty = 6553;  // 約2ms（180°）
  // 角度をパルス幅に変換（0°-180°を0-145に換算したうえで変換：サーボハットの仕様から145度±10度が限度のため）
  int duty = map(angle, 0, 145, minDuty, maxDuty);
  // デューティーサイクルをPWM信号として出力
  ledcWrite(ch, duty);
}

// ----------------------------------------------------------------
// センサー関連

// センサーデータ用変数
float temperature = 0.0;   // 気温
float humidity = 0.0;      // 湿度
float pressure = 1000.0;   // 気圧
uint16_t co2 = 400;        // CO2濃度
uint16_t discomfort = 70;  // 不快指数
uint16_t angle = 180;      // 設定角度

#define ENV_III // ENV_III使用しない場合はコメントアウト
#define SGP30   // SGP30使用しない場合はコメントアウト

// ENV_III : 温度湿度気圧センサー
#ifdef ENV_III
#include <M5UnitENV.h>
SHT3X sht30;
QMP6988 qmp6988;

// float tmp = 0.0;
// float hum = 0.0;
// float pressure = 0.0;
#endif

// SGP30 : ガスセンサー、CO2測定
#ifdef SGP30
#include <Adafruit_SGP30.h>
Adafruit_SGP30 sgp;
uint16_t eco2_base = 37335; // eco2 baseline仮設定値
uint16_t tvoc_base = 40910; // TVOC baseline仮設定値
#endif

// ENV_IIIで取得した温湿度を元に絶対湿度に補正
uint32_t getAbsoluteHumidity()
{
  // approximation formula from Sensirion SGP30 Driver Integration chapter 3.15
  const float absoluteHumidity = 216.7f * ((humidity / 100.0f) * 6.112f * exp((17.62f * temperature) / (243.12f + temperature)) / (273.15f + temperature)); // [g/m^3]
  const uint32_t absoluteHumidityScaled = static_cast<uint32_t>(1000.0f * absoluteHumidity);                                                                // [mg/m^3]
  return absoluteHumidityScaled;
}

// 不快指数の計算
/**
 * ～55   : 寒い
 * 55～60 : 肌寒い
 * 60～65 : 何も感じない
 * 65～70 : 快い
 * 70～75 : 暑くない
 * 75～80 : やや暑い
 * 80～85 : 暑くて汗が出る
 * 85～   : 暑くてたまらない
 * 出典参考： https://www.calc-site.com/healths/discomfort_index
 */
int getDiscomfortIndex()
{
  float discomfortIndex = 0.81 * temperature + 0.01 * humidity * (0.99 * temperature - 14.3) + 46.3;
  return (int)discomfortIndex;
}

// 不快指数値によるアバターのセット（顔色、表情）
// 色見本 : https://rgbcolorpicker.com/565/table
void setComfortLevelExpression() {
    if (discomfort >= 50 && discomfort < 55) {
      // 寒い
      avatar.setExpression(Expression::Sad);
      cp->set(COLOR_PRIMARY, TFT_WHITE);
      cp->set(COLOR_BACKGROUND, TFT_BLUE);
    } else if (discomfort >= 55 && discomfort < 60) {
      // 肌寒い
      avatar.setExpression(Expression::Doubt);
      cp->set(COLOR_PRIMARY, TFT_WHITE);
      cp->set(COLOR_BACKGROUND, TFT_SKYBLUE);
    } else if (discomfort >= 60 && discomfort < 65) {
      // 何も感じない
      avatar.setExpression(Expression::Neutral);
      cp->set(COLOR_PRIMARY, TFT_BLACK);
      cp->set(COLOR_BACKGROUND, TFT_WHITE);
    } else if (discomfort >= 65 && discomfort < 70) {
      // 快い
      avatar.setExpression(Expression::Happy);
      cp->set(COLOR_PRIMARY, TFT_BLACK);
      cp->set(COLOR_BACKGROUND, 65531); // lightyellow : 65531
    } else if (discomfort >= 70 && discomfort < 75) {
      // 暑くない
      avatar.setExpression(Expression::Neutral);
      cp->set(COLOR_PRIMARY, TFT_WHITE);
      cp->set(COLOR_BACKGROUND, TFT_GOLD);
    } else if (discomfort >= 75 && discomfort < 80) {
      // やや暑い
      avatar.setExpression(Expression::Doubt);
      cp->set(COLOR_PRIMARY, TFT_WHITE);
      cp->set(COLOR_BACKGROUND, TFT_ORANGE);
    } else if (discomfort >= 80 && discomfort < 85) {
      // 暑くて汗が出る
      avatar.setExpression(Expression::Angry);
      cp->set(COLOR_PRIMARY, TFT_WHITE);
      cp->set(COLOR_BACKGROUND, TFT_MAROON);
    } else {
      // 範囲外の値(デフォルト)
      cp->set(COLOR_PRIMARY, TFT_WHITE);
      cp->set(COLOR_BACKGROUND, TFT_GOLD);
    }
    avatar.setColorPalette(*cp);
}

// ----------------------------------------------------------------
// タイマー
// ----------------------------------------------------------------
hw_timer_t *timer = NULL;  // タイマー設定
volatile bool timerFlag = true;  // タイマー割り込みが発生したことを示すフラグ

// タイマー割り込み関数
void IRAM_ATTR onTimer() {
  timerFlag = true;  // フラグを立てる（定期的な処理の実行を指示）
}

// ----------------------------------------------------------------
// メイン（setup, loop）
// ----------------------------------------------------------------
void setup() {
  auto cfg = M5.config();     // 設定用の情報を抽出
  M5.begin(cfg);              // M5Stackをcfgの設定で初期化

// センサー関連
// ENV_III : 温度湿度気圧センサー
#ifdef ENV_III
  Wire.begin(32, 33); // Wireの初期化、I2Cバスを追加。
  if (!qmp6988.begin(&Wire, QMP6988_SLAVE_ADDRESS_L, 32, 33, 400000U))
  {
    M5.Lcd.println("Couldn't find QMP6988 :(");
    while (1)
      delay(1);
  }

  if (!sht30.begin(&Wire, SHT3X_I2C_ADDR, 32, 33, 400000U))
  {
    M5.Lcd.println("Couldn't find SHT3X :(");
    while (1)
      delay(1);
  }

#endif

// SGP30 : ガスセンサー、CO2測定
#ifdef SGP30
  if (!sgp.begin())
  {
    Serial.println("Couldn't find SGP30 :(");
    while (1)
      ;
  }
  sgp.softReset();
  sgp.IAQinit();
  sgp.setIAQBaseline(eco2_base, tvoc_base); // 仮のbaseline設定しない場合はコメントアウト要
  for (int i = 0; i < 15; i++)
  { // SGP30が動作するまで15秒ウェイト
    M5.Lcd.printf(".");
    delay(1000);
  }
#endif

  // アバター関連（Position, Scalse は m5stick-c plus用）
  // 顔ポジション設定（m5stick-c plus用）
  M5.Lcd.setRotation(1);
  avatar.setScale(0.6);
  avatar.setPosition(-60, -45);
  cp = new ColorPalette();
  cp->set(COLOR_PRIMARY, TFT_WHITE);
  cp->set(COLOR_BACKGROUND, TFT_GOLD);
  avatar.setColorPalette(*cp);
  avatar.init(8);
  avatar.setSpeechFont(&fonts::lgfxJapanGothicP_16);

  // サーボハット関連
  ledcSetup(ch, pwm_hz, timer_width);
  ledcAttachPin(pin, ch);
  setServoAngle(180); // 初期位置は一番低い位置
  delay(2000);

  // タイマー設定
  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 30000000, true); // だいたい30秒間隔でタイマーが発動 (設定値はマイクロ秒)
  timerAlarmEnable(timer);
}

// ----------------------------------------------------------------
char speechText[100];  // フォーマットされた文字列を格納するためのバッファ
// ----------------------------------------------------------------
void loop() {

  M5.update();

  // 定期的に実行される処理
  if (timerFlag) {
// ENV_IIIデータ取得
#ifdef ENV_III
    // 気圧を測定し構造体にセット
    pressure = qmp6988.calcPressure() / 100;
    if (sht30.update())
    {
      // 温度・湿度を測定し構造体にセット
      temperature = sht30.cTemp;
      humidity = sht30.humidity;
    }
#endif

// SGP30データ取得
#ifdef SGP30
    if (!sgp.IAQmeasure())
    { // eCo2 TVOC読込
      Serial.println("Measurement failed");
      while (1)
        delay(1);
    }
    // CO2濃度を測定し構造体にセット
    co2 = sgp.eCO2;
#endif

// ENV_IIIで取得した温湿度を元に補正した絶対湿度をSGP30にセット
#if defined(SGP30) && defined(ENV_III)
    sgp.setHumidity(getAbsoluteHumidity());
#endif

    // 不快指数の計算
    // 50:寒さ上限
    // 68:快適の中心
    // 85:暑さ下限
    discomfort = getDiscomfortIndex();
    // 不快指数が限界値以上/以下の場合は限界値にリセット
    if (discomfort < 50) discomfort = 50;
    if (discomfort > 85) discomfort = 85;

    // 不快指数を角度に変換：サーボの回転方向を考慮し不快指数の高低と角度の高低を反転
    angle = 180 - map(discomfort, 50, 85, 0, 180);
    setServoAngle(angle);

    // 表情と顔色の変更
    setComfortLevelExpression();

    // タイマー割込みのフラグをリセット
    timerFlag = false;
  }

  // 常時実行される処理
  // 現在のセンサーデータの表示
// ENV_IIIデータ取得
#ifdef ENV_III
  sprintf(speechText, "気温 : %2.1f 'C", temperature);
  avatar.setSpeechText(speechText);
  delay(3000);
  sprintf(speechText, "湿度 : %2.1f %c ", humidity, '%');
  avatar.setSpeechText(speechText);
  delay(3000);
  sprintf(speechText, "気圧 : %4.1f hPa", pressure);
  avatar.setSpeechText(speechText);
  delay(3000);
#endif
// SGP30データ取得
#ifdef SGP30
  sprintf(speechText, "CO2 : %d ppm", co2);
  avatar.setSpeechText(speechText);
  delay(3000);
#endif
  sprintf(speechText, "不快指数 : %d", discomfort);
  avatar.setSpeechText(speechText);
  delay(3000);
  // sprintf(speechText, "角度 : %d", angle);
  // avatar.setSpeechText(speechText);
  // delay(3000);

}
