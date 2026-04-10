// WT32-ETH01 announcement player pin map

// I2S -> PCM5102A
static const int PIN_I2S_BCK   = 14;
static const int PIN_I2S_LRCK  = 15;
static const int PIN_I2S_DOUT  = 12;

// SPI -> microSD
static const int PIN_SD_SCK    = 17;
static const int PIN_SD_MOSI   =  5;
static const int PIN_SD_MISO   = 36;
static const int PIN_SD_CS     =  4;
static const int SD_SPI_HZ     = 1000000;

// I2C -> MCP23017
static const int PIN_I2C_SDA   = 32;
static const int PIN_I2C_SCL   = 33;
static const int PIN_MCP_INT   = 39;   // optional

// Ethernet PHY -> LAN8720
static const int PIN_ETH_ADDR  = 1;
static const int PIN_ETH_POWER = 16;
static const int PIN_ETH_MDC   = 23;
static const int PIN_ETH_MDIO  = 18;

// Spare
static const int PIN_BUTTON_1  = 35;
static const int PIN_BUTTON_2  = 2;