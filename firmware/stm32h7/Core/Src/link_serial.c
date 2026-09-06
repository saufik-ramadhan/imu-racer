#include "link_serial.h"

#include <stdio.h>
#include <string.h>

#define TX_TIMEOUT_MS      20U
#define HOST_TIMEOUT_MS    2000U
#define RX_RING_SIZE       64U      /* power of two */
#define MAX_PAYLOAD        32U

typedef struct __attribute__((packed)) {
  int16_t  steer;
  int16_t  pitch;
  uint8_t  buttons;
  uint8_t  flags;
  uint16_t seq;
} telemetry_packet_t;

/* The BSP owns the UART; COM1 is USART3, wired to the ST-LINK VCP. */
static UART_HandleTypeDef *s_uart;

static volatile uint8_t  s_ring[RX_RING_SIZE];
static volatile uint16_t s_head;      /* written by the ISR  */
static volatile uint16_t s_tail;      /* read by the polling side */

static const char *s_device_name = "IMU Racer";
static uint16_t s_seq;
static bool     s_zero_requested;
static uint32_t s_host_last_seen;
static bool     s_host_seen_once;

/* -------------------------------------------------------------------------- */
/*                                  Framing                                   */
/* -------------------------------------------------------------------------- */

static uint8_t crc8(const uint8_t *data, uint16_t len)
{
  uint8_t crc = 0x00;

  for (uint16_t i = 0; i < len; i++)
  {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++)
    {
      crc = (uint8_t)((crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1));
    }
  }
  return crc;
}

static void send_frame(uint8_t type, const void *payload, uint8_t len)
{
  uint8_t frame[3 + MAX_PAYLOAD + 1];

  if (s_uart == NULL || len > MAX_PAYLOAD)
  {
    return;
  }

  frame[0] = LINK_SOF;
  frame[1] = type;
  frame[2] = len;
  if (len != 0)
  {
    memcpy(&frame[3], payload, len);
  }
  frame[3 + len] = crc8(&frame[1], (uint16_t)(len + 2));

  HAL_UART_Transmit(s_uart, frame, (uint16_t)(len + 4), TX_TIMEOUT_MS);
}

/* -------------------------------------------------------------------------- */
/*                                  Receive                                   */
/* -------------------------------------------------------------------------- */

/*
 * RX is handled at register level rather than through HAL_UART_Receive_IT.
 * printf() goes out of the same handle with the blocking HAL transmit, and
 * restarting a HAL receive from inside an interrupt while the main loop sits
 * in that transmit is exactly the kind of state clash that silently stops
 * reception. Reading RDR here touches nothing the transmit path owns.
 */
void USART3_IRQHandler(void)
{
  USART_TypeDef *u = s_uart->Instance;
  uint32_t isr = u->ISR;

  /* An overrun would otherwise latch and stop further receive interrupts. */
  if ((isr & USART_ISR_ORE) != 0U)
  {
    u->ICR = USART_ICR_ORECF;
  }

  while ((u->ISR & USART_ISR_RXNE_RXFNE) != 0U)
  {
    uint8_t byte = (uint8_t)(u->RDR & 0xFFU);
    uint16_t next = (uint16_t)((s_head + 1U) % RX_RING_SIZE);

    if (next != s_tail)         /* drop on overflow rather than overwrite */
    {
      s_ring[s_head] = byte;
      s_head = next;
    }
  }
}

static bool ring_pop(uint8_t *out)
{
  if (s_tail == s_head)
  {
    return false;
  }
  *out = s_ring[s_tail];
  s_tail = (uint16_t)((s_tail + 1U) % RX_RING_SIZE);
  return true;
}

static void handle_frame(uint8_t type, const uint8_t *payload, uint8_t len)
{
  s_host_last_seen = HAL_GetTick();

  switch (type)
  {
  case LINK_MSG_HELLO:
    /* A browser tab has just opened the port, or is keeping it alive. */
    if (!s_host_seen_once)
    {
      s_host_seen_once = true;
      printf("link: host attached\r\n");
    }
    link_serial_send_info();
    break;

  case LINK_MSG_CMD:
    if (len >= 1 && payload[0] == LINK_CMD_ZERO)
    {
      /* Latched, not acted on here: the control loop owns the tilt filter. */
      s_zero_requested = true;
    }
    break;

  default:
    break;
  }
}

void link_serial_poll(void)
{
  /* Parser state survives between calls -- frames arrive a byte at a time. */
  static enum { WAIT_SOF, WAIT_TYPE, WAIT_LEN, WAIT_PAYLOAD, WAIT_CRC } state = WAIT_SOF;
  static uint8_t type;
  static uint8_t len;
  static uint8_t got;
  static uint8_t payload[MAX_PAYLOAD];

  uint8_t byte;

  while (ring_pop(&byte))
  {
    switch (state)
    {
    case WAIT_SOF:
      if (byte == LINK_SOF)
      {
        state = WAIT_TYPE;
      }
      break;

    case WAIT_TYPE:
      type = byte;
      state = WAIT_LEN;
      break;

    case WAIT_LEN:
      len = byte;
      got = 0;
      if (len > MAX_PAYLOAD)
      {
        state = WAIT_SOF;      /* not ours -- resynchronise */
      }
      else
      {
        state = (len == 0) ? WAIT_CRC : WAIT_PAYLOAD;
      }
      break;

    case WAIT_PAYLOAD:
      payload[got++] = byte;
      if (got == len)
      {
        state = WAIT_CRC;
      }
      break;

    case WAIT_CRC:
    default:
    {
      uint8_t check[2 + MAX_PAYLOAD];

      check[0] = type;
      check[1] = len;
      if (len != 0)
      {
        memcpy(&check[2], payload, len);
      }
      if (crc8(check, (uint16_t)(len + 2)) == byte)
      {
        handle_frame(type, payload, len);
      }
      state = WAIT_SOF;
      break;
    }
    }
  }

  if (s_host_seen_once && !link_serial_host_present())
  {
    s_host_seen_once = false;
    printf("link: host went quiet\r\n");
  }
}

/* -------------------------------------------------------------------------- */
/*                                   Public                                   */
/* -------------------------------------------------------------------------- */

void link_serial_init(const char *device_name)
{
  if (device_name != NULL)
  {
    s_device_name = device_name;
  }
  s_uart = &hcom_uart[COM1];

  s_head = 0;
  s_tail = 0;

  /* BSP_COM_Init() brings the UART up but leaves it transmit-only. */
  __HAL_UART_ENABLE_IT(s_uart, UART_IT_RXNE);
  HAL_NVIC_SetPriority(USART3_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(USART3_IRQn);

  link_serial_send_info();
}

void link_serial_send_info(void)
{
  send_frame(LINK_MSG_INFO, s_device_name, (uint8_t)strlen(s_device_name));
}

void link_serial_publish(float steer, float pitch, uint8_t buttons, bool zeroed)
{
  telemetry_packet_t pkt;

  if (steer < -1.0f) { steer = -1.0f; }
  if (steer >  1.0f) { steer =  1.0f; }
  if (pitch < -1.0f) { pitch = -1.0f; }
  if (pitch >  1.0f) { pitch =  1.0f; }

  pkt.steer   = (int16_t)(steer * 1000.0f);
  pkt.pitch   = (int16_t)(pitch * 1000.0f);
  pkt.buttons = buttons;
  pkt.flags   = zeroed ? 0x01 : 0x00;
  pkt.seq     = s_seq++;

  send_frame(LINK_MSG_TELEMETRY, &pkt, sizeof(pkt));
}

bool link_serial_take_zero_request(void)
{
  if (!s_zero_requested)
  {
    return false;
  }
  s_zero_requested = false;
  return true;
}

bool link_serial_host_present(void)
{
  if (s_host_last_seen == 0U)
  {
    return false;
  }
  return (HAL_GetTick() - s_host_last_seen) < HOST_TIMEOUT_MS;
}
