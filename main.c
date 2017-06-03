// Alien Advance
// Michael Ebens

// Controls:
// DPAD to move ship
// Right button to shoot
// Right potentiometer to control aim

// Console controls:
// WASD to move ship
// Space to shoot

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#include <cpu_speed.h>
#include <lcd.h>
#include <graphics.h>
#include <sprite.h>

#include "usb_serial.h"

#define PI 3.141592653589

// bit operations

#define BIT_OFF(port, pin) port &= ~(1 << pin)
#define BIT_ON(port, pin) port |= (1 << pin)
#define BIT_FLIP(port, pin) port ^= (1 << pin)
#define GET_BIT(port, pin) (port >> pin) & 1
#define SET_BIT(port, pin, val) port = (port & ~(1 << pin)) | (val << pin)

// input definitions

#define NUM_BUTTONS 7
#define BTN_DPAD_LEFT 0
#define BTN_DPAD_RIGHT 1
#define BTN_DPAD_UP 2
#define BTN_DPAD_DOWN 3
#define BTN_DPAD_CENTER 4
#define BTN_LEFT 5
#define BTN_RIGHT 6

#define BTN_STATE_UP 0
#define BTN_STATE_DOWN 1

// input states

volatile unsigned char btn_hists[NUM_BUTTONS];
volatile unsigned char btn_states[NUM_BUTTONS];
volatile unsigned int press_count = 0;
volatile unsigned char btn_right_pressed = 0;

unsigned char usb_left = 0;
unsigned char usb_right = 0;
unsigned char usb_up = 0;
unsigned char usb_down = 0;
unsigned char usb_shoot = 0;

// -1 = waiting for/connected to USB
// 0 = intro
// 1 = countdown
// 2 = gameplay
// 3 = game over
char GAME_STATE = -1;
unsigned char mothership_battle = 0;
unsigned char lives = 10;
unsigned int score = 0;
unsigned char countdown = 4;

// timers

float time = 0;
float last_clock = 0;
float DT;
float light_timer = 0;
float debug_timer = 0.5;
float input_timer = 0;
float first_input_time = 0;
unsigned int clock_overflow = 0;

// 7812.5 = 8MHz / 1024 (freq / prescaler)
#define TIMER1_TIME (1 / 7812.5)
#define TIMER1_OVERFLOW (TIMER1_TIME * 0xFFFF)

// player 

#define PWIDTH 5
#define PHEIGHT 5

unsigned char player_bitmap[] = {
  0b11111000,
  0b11011000,
  0b10001000,
  0b11011000,
  0b11111000
};

Sprite player;

// enemies

#define NUM_ENEMIES 6
#define EWIDTH 5
#define EHEIGHT 5

unsigned char enemy_bitmap[] = {
  0b10001000,
  0b01010000,
  0b10101000,
  0b01010000,
  0b10001000
};

Sprite enemies[NUM_ENEMIES];
float enemy_timers[NUM_ENEMIES];
unsigned char enemies_alive = NUM_ENEMIES;

// mothership

#define MSWIDTH 12
#define MSHEIGHT 12

unsigned char mothership_bitmap[] = {
  0b00101111, 0b01000000,
  0b01001111, 0b00100000,
  0b10011111, 0b10010000,
  0b11001111, 0b00110000,
  0b10110000, 0b11010000,
  0b11100110, 0b01110000,
  0b11100110, 0b01110000,
  0b10110000, 0b11010000,
  0b11001111, 0b00110000,
  0b10011111, 0b10010000,
  0b01001111, 0b00100000,
  0b00101111, 0b01000000
};

Sprite mothership;
Sprite mother_missile;

#define MOTHER_MAX_HEALTH 15
unsigned char mother_health = MOTHER_MAX_HEALTH;
float mother_move_timer;
float mother_shoot_timer;

// missiles

#define NUM_MISSILES 5
#define MWIDTH 2
#define MHEIGHT 2

unsigned char missile_bitmap[] = {
  0b1100000,
  0b1100000
};

Sprite missiles[NUM_MISSILES];

// character buffers

char buff[80];
char time_buff[18];

float get_system_time()
{
  return (clock_overflow * 65536 + TCNT1) * TIMER1_TIME;
}

float get_system_clock()
{
  return TIMER1_TIME * TCNT1;
}

void send_debug_string(char* string)
{
    // Send the debug preamble...
    sprintf(time_buff, "[DEBUG @ %6.3f] ", (double) get_system_time());
    usb_serial_write((const uint8_t *) time_buff, 18);
    
    // Send all of the characters in the string
    while (*string != '\0')
    {
        usb_serial_putchar(*string);
        string++;
    }

    // Go to a new line (force this to be the start of the line)
    usb_serial_putchar('\r');
    usb_serial_putchar('\n');
}

void init()
{
  // inputs
  DDRF &= 0b10011100; // SW1, SW2, ADC1, ADC0
  DDRB &= 0b01111100; // SWA, SWB, SWCENTER
  DDRD &= 0b11111100; // SWC, SWD

  // outputs
  DDRB |= 0b00001100; // LED1, LED0

  // timer0 - debouncing timer
  // CTC mode, uses OCR0A
  BIT_OFF(TCCR0A, WGM00);
  BIT_ON(TCCR0A, WGM01);
  BIT_OFF(TCCR0B, WGM02);
  OCR0A = 94; // overflow is 3.008ms (due to OCR0A)

  // prescaler 256
  BIT_ON(TCCR0B, CS02);
  BIT_OFF(TCCR0B, CS01);
  BIT_OFF(TCCR0B, CS00);

  // overflow interrupt
  BIT_ON(TIMSK0, OCIE0A);

  // timer1 - system clock timer
  // normal mode
  BIT_OFF(TCCR1A, WGM10);
  BIT_OFF(TCCR1A, WGM11);
  BIT_OFF(TCCR1B, WGM12);
  BIT_OFF(TCCR1B, WGM13);

  // prescaler 1024, overflow is 8.388608
  BIT_ON(TCCR1B, CS10);
  BIT_OFF(TCCR1B, CS11);
  BIT_ON(TCCR1B, CS12);

  // overflow interrupt
  BIT_ON(TIMSK1, TOIE1);

  // USB
  usb_init();

  // enable interrupts
  sei();

  // ADC

  ADMUX = 0;
  BIT_ON(ADMUX, REFS0); // AREF = AVcc
  BIT_ON(ADMUX, MUX0); // enable ADC1 (right-hand side)

  // Enable ADC
  BIT_ON(ADCSRA, ADEN);
  // pre-scaler of 128 (8000000/128 = 62500)
  BIT_ON(ADCSRA, ADPS2);
  BIT_ON(ADCSRA, ADPS1);
  BIT_ON(ADCSRA, ADPS0);

  // screen
  lcd_init(LCD_DEFAULT_CONTRAST); 
  show_screen();

  // player
  init_sprite(&player, 39, 28, PWIDTH, PHEIGHT, player_bitmap);

  // enemies
  for (unsigned char i = 0; i < NUM_ENEMIES; i++)
  {
    // place at (0, 0) initially
    init_sprite(&enemies[i], 0, 0, EWIDTH, EHEIGHT, enemy_bitmap);
  }

  // mothership
  init_sprite(&mothership, 0, 0, MSWIDTH, MSHEIGHT, mothership_bitmap);
  init_sprite(&mother_missile, 0, 0, MWIDTH, MHEIGHT, missile_bitmap);

  // missiles
  for (unsigned char i = 0; i < NUM_MISSILES; i++)
  {
    init_sprite(&missiles[i], 0, 0, MWIDTH, MHEIGHT, missile_bitmap);
  }
}

void display_intro()
{
  draw_string(10, 0, "Alien Advance");
  draw_string(9, 12, "Michael Ebens");
  draw_string(22, 20, "n9732080");
  draw_string(7, 32, "Press a button");
  draw_string(7, 40, "to continue...");
}

void draw_border()
{
  draw_line(0, 8, 0, 47); // left
  draw_line(0, 8, 83, 8); // top
  draw_line(83, 8, 83, 47); // right
  draw_line(0, 47, 83, 47); // bottom
}

void draw_status()
{
  unsigned char mins = floor(time / 60);
  sprintf(buff, "S:%d L:%d T:%02d:%02d", score, lives, mins, (unsigned char) floor(time - mins * 60));
  draw_string(0, 0, buff);
}

void find_empty_position(unsigned char* x, unsigned char* y, unsigned char width, unsigned char height, unsigned char check_player)
{
  unsigned char okay = 1;

  while (1)
  {
    *x = 1 + (82 - width) * (((float) rand()) / RAND_MAX);
    *y = 9 + (38 - height) * (((float) rand()) / RAND_MAX);
    okay = 1;

    if (check_player && !(*x >= player.x + PWIDTH + 2 || *x + width <= player.x - 2 || *y >= player.y + PHEIGHT + 2 || *y + height <= player.y - 2))
    {
      okay = 0;
    }

    if (okay)
    {
      for (unsigned char i = 0; i < NUM_ENEMIES; i++)
      {
        // rect-to-rect collision with enemies padded to prevent immediate collision
        if (!(*x >= enemies[i].x + EWIDTH + 2 || *x + width <= enemies[i].x - 2 || *y >= enemies[i].y + EHEIGHT + 2 || *y + height <= enemies[i].y - 2))
        {
          okay = 0;
          break;
        }
      }

      if (okay) break;
    }
  }
}

void reset_enemies(unsigned char check_player)
{
  // reset all enemies to top, so they don't interfere when finding empty positions
  for (unsigned char i = 0; i < NUM_ENEMIES; i++)
  {
    enemies[i].x = 0;
    enemies[i].y = 0;
  }

  unsigned char x;
  unsigned char y;

  for (unsigned char i = 0; i < NUM_ENEMIES; i++)
  {
    find_empty_position(&x, &y, EWIDTH, EHEIGHT, check_player);
    enemies[i].x = x;
    enemies[i].y = y;
    enemies[i].is_visible = 1;
    enemy_timers[i] = 2 + 2 * (((float) rand()) / RAND_MAX);
  }

  enemies_alive = NUM_ENEMIES;
}

float get_shooting_angle()
{
  BIT_ON(ADCSRA, ADSC); // start conversion
  while (GET_BIT(ADCSRA, ADSC));  // wait until complete
  return ((float) ADC) / 1023 * PI * 4;
}

int main(void)
{
  set_clock_speed(CPU_8MHz);

  init();

  while (1)
  {
    // calculate delta time
    DT = get_system_clock() - last_clock;
    if (DT < 0) DT += TIMER1_OVERFLOW;
    last_clock = get_system_clock();

    // random seed by measuring the time taken to the first button press
    if (first_input_time != -1)
    {
      first_input_time += DT;

      for (unsigned char i = 0; i < NUM_BUTTONS; i++)
      {
        if (i == BTN_DPAD_CENTER) continue; // this'll be active by default at startup

        if (btn_states[i] == BTN_STATE_DOWN)
        {
          srand(first_input_time);
          first_input_time = -1;
        }
      }
    }

    if (light_timer > 0)
    {
      BIT_ON(PORTB, 2);
      BIT_ON(PORTB, 3);
      light_timer -= DT;

      if (light_timer <= 0)
      {
        BIT_OFF(PORTB, 2);
        BIT_OFF(PORTB, 3);
      }
    }

    clear_screen();

    if (GAME_STATE == -1)
    {
      draw_string(14, 15, "Waiting for");
      draw_string(7, 26, "USB connection");
      show_screen();
      while(!usb_configured() || !usb_serial_get_control());

      clear_screen();
      draw_string(7, 20, "USB connected!");
      show_screen();
      GAME_STATE = 0;
      send_debug_string("Greetings! You are connected via USB to Alien Advance.");
      send_debug_string("Use the WASD keys to move player and press space to shoot.");
      _delay_ms(500);
    }
    else if (GAME_STATE == 0)
    {
      display_intro();

      if (input_timer > 0)
      {
        input_timer -= DT;
      }
      else if (btn_states[BTN_LEFT] == BTN_STATE_DOWN || btn_states[BTN_RIGHT] == BTN_STATE_DOWN)
      {
        GAME_STATE = 1;
        countdown = 4;
        continue; // skips initial 300ms delay
      }
    }
    else if (GAME_STATE == 1)
    {
      if (countdown > 1)
      {
        countdown--;
      }
      else
      {
        // ROUND INITIALISATION

        GAME_STATE = 2;
        time = 0;
        score = 0;
        lives = 5;
        mothership_battle = 0;
        last_clock = get_system_clock();

        reset_enemies(0);
        mothership.is_visible = 0;
        mother_missile.is_visible = 0;

        // find position for player
        unsigned char x;
        unsigned char y;
        find_empty_position(&x, &y, PWIDTH, PHEIGHT, 0);
        player.x = x;
        player.y = y;

        // make sure all missiles are invisible
        for (unsigned char i = 0; i < NUM_MISSILES; i++)
        {
          missiles[i].is_visible = 0;
        }
      }

      sprintf(buff, "%1d", countdown);
      draw_string(39, 20, buff);
    }
    else if (GAME_STATE == 2)
    {
      time += DT;
      float player_angle = get_shooting_angle();

      if (debug_timer > 0)
      {
        debug_timer -= DT;
      }
      else
      {
        sprintf(buff, "Player's current position: (%d, %d)", (unsigned char) player.x, (unsigned char) player.y);
        send_debug_string(buff);
        sprintf(buff, "Player's current aim: %.1f", player_angle * (180 / PI));
        send_debug_string(buff);
        debug_timer = 0.5;
      }

      usb_left = 0;
      usb_right = 0;
      usb_up = 0;
      usb_down = 0;
      usb_shoot = 0;
      int16_t usb_char = usb_serial_getchar();

      while (usb_char != -1)
      {
        switch (usb_char)
        {
          case 'a':
            usb_left = 1;
            break;

          case 'd':
            usb_right = 1;
            break;

          case 'w':
            usb_up = 1;
            break;

          case 's':
            usb_down = 1;
            break;

          case ' ':
            usb_shoot = 1;
            break;
        }

        usb_char = usb_serial_getchar();
      }

      if (mothership_battle)
      {
        // mothership

        unsigned char end_path = 0;

        if (mother_move_timer > 0)
        {
          mother_move_timer -= DT;
        }
        else
        {
          if (!mothership.dx)
          {
            float angle = atan2(player.y + PHEIGHT / 2 - (mothership.y + MSHEIGHT / 2), player.x + PWIDTH / 2 - (mothership.x + MSWIDTH / 2));
            mothership.dx = 2 * cos(angle);
            mothership.dy = 2 * sin(angle);
          }

          mothership.x += mothership.dx * DT;
          mothership.y += mothership.dy * DT;

          if (mothership.x < 1)
          {
            mothership.x = 1;
            end_path = 1;
          }
          else if (mothership.x > 83 - MSWIDTH)
          {
            mothership.x = 83 - MSWIDTH;
            end_path = 1;
          }

          if (mothership.y < 9)
          {
            mothership.y = 9;
            end_path = 1;
          }
          else if (mothership.y > 47 - MSHEIGHT)
          {
            mothership.y = 47 - MSHEIGHT;
            end_path = 1;
          }
        }

        draw_sprite(&mothership);

        unsigned char health_x = mothership.x + floor((MSWIDTH - 1) * ((float) mother_health / MOTHER_MAX_HEALTH));
        unsigned char health_y = mothership.y < 14 ? mothership.y + MSHEIGHT + 1 : mothership.y - 3;

        draw_line(mothership.x, health_y,  health_x, health_y); 
        draw_line(mothership.x, health_y + 1, health_x, health_y + 1); 

        if (!(mothership.x >= player.x + PWIDTH || mothership.x + MSWIDTH <= player.x
            || mothership.y >= player.y + PHEIGHT || mothership.y + MSHEIGHT <= player.y))
        {
          if (mother_move_timer <= 0) end_path = 1;
          lives--;
          send_debug_string("Mothership destroyed the player");

          if (lives <= 0)
          {
            GAME_STATE = 3;
          }
          else
          {
            unsigned char x;
            unsigned char y;
            find_empty_position(&x, &y, PWIDTH, PHEIGHT, 0);
            player.x = x;
            player.y = y;
            light_timer = 0.5;
          }
        }

        if (end_path)
        {
          mothership.dx = 0;
          mothership.dy = 0;
          mother_move_timer = 2 + 2 * (((float) rand()) / RAND_MAX); 
        }

        if (mother_shoot_timer > 0)
        {
          mother_shoot_timer -= DT;
        }
        else if (!mother_missile.is_visible)
        {
          float angle = atan2(player.y + PHEIGHT / 2 - (mothership.y + MSHEIGHT / 2), player.x + PWIDTH / 2 - (mothership.x + MSWIDTH / 2));
          mother_missile.x = mothership.x + MSWIDTH / 2 + 4 * cos(angle);
          mother_missile.y = mothership.y + MSHEIGHT / 2 + 4 * sin(angle);
          mother_missile.dx = 10 * cos(angle);
          mother_missile.dy = 10 * sin(angle);
          mother_missile.is_visible = 1;
          mother_shoot_timer = 2 + 2 * (((float) rand()) / RAND_MAX);
        }

        if (mother_missile.is_visible)
        {
          mother_missile.x += mother_missile.dx * DT;
          mother_missile.y += mother_missile.dy * DT;

          if (mother_missile.x < 1 || mother_missile.x > 83 - MWIDTH || mother_missile.y < 9 || mother_missile.y > 47 - MHEIGHT)
          {
            mother_missile.is_visible = 0;
            continue;
          }

          if (!(mother_missile.x >= player.x + PWIDTH || mother_missile.x + MWIDTH <= player.x
              || mother_missile.y >= player.y + PHEIGHT || mother_missile.y + MHEIGHT <= player.y))
          {
            mother_missile.is_visible = 0;
            lives--;
            send_debug_string("Mothership destroyed the player");

            if (lives <= 0)
            {
              GAME_STATE = 3;
            }
            else
            {
              unsigned char x;
              unsigned char y;
              find_empty_position(&x, &y, PWIDTH, PHEIGHT, 0);
              player.x = x;
              player.y = y;
              light_timer = 0.5;
            }
          }

          draw_sprite(&mother_missile);
        }
      }
      else
      {
        // enemies

        for (unsigned char i = 0; i < NUM_ENEMIES; i++)
        {
          if (!enemies[i].is_visible) continue;
          unsigned char end_path = 0;

          if (enemy_timers[i] > 0)
          {
            enemy_timers[i] -= DT;
          }
          else
          {
            if (!enemies[i].dx)
            {
              float angle = atan2(player.y + PHEIGHT / 2 - (enemies[i].y + EHEIGHT / 2), player.x + PWIDTH / 2 - (enemies[i].x + EWIDTH / 2));
              enemies[i].dx = 4 * cos(angle);
              enemies[i].dy = 4 * sin(angle);
            }

            enemies[i].x += enemies[i].dx * DT;
            enemies[i].y += enemies[i].dy * DT;

            if (enemies[i].x < 1)
            {
              enemies[i].x = 1;
              end_path = 1;
            }
            else if (enemies[i].x > 83 - EWIDTH)
            {
              enemies[i].x = 83 - EWIDTH;
              end_path = 1;
            }

            if (enemies[i].y < 9)
            {
              enemies[i].y = 9;
              end_path = 1;
            }
            else if (enemies[i].y > 47 - EHEIGHT)
            {
              enemies[i].y = 47 - EHEIGHT;
              end_path = 1;
            }
          }

          if (!(enemies[i].x >= player.x + PWIDTH || enemies[i].x + EWIDTH <= player.x
              || enemies[i].y >= player.y + PHEIGHT || enemies[i].y + EHEIGHT <= player.y))
          {
            if (enemy_timers[i] <= 0) end_path = 1;
            lives--;
            send_debug_string("Alien killed the player");

            if (lives <= 0)
            {
              GAME_STATE = 3;
            }
            else
            {
              unsigned char x;
              unsigned char y;
              find_empty_position(&x, &y, PWIDTH, PHEIGHT, 0);
              player.x = x;
              player.y = y;
              light_timer = 0.5;
            }
          }

          if (end_path)
          {
            enemies[i].dx = 0;
            enemies[i].dy = 0;
            enemy_timers[i] = 2 + 2 * (((float) rand()) / RAND_MAX); 
          }

          draw_sprite(&enemies[i]);
        }
      }
      
      // player

      char x_axis = 0;
      char y_axis = 0;

      if (btn_states[BTN_DPAD_LEFT] == BTN_STATE_DOWN || usb_left) x_axis--;
      if (btn_states[BTN_DPAD_RIGHT] == BTN_STATE_DOWN || usb_right) x_axis++;
      if (btn_states[BTN_DPAD_UP] == BTN_STATE_DOWN || usb_up) y_axis--;
      if (btn_states[BTN_DPAD_DOWN] == BTN_STATE_DOWN || usb_down) y_axis++;

      if (x_axis != 0)
      {
        player.x += 12 * x_axis * DT;
        if (player.x < 1) player.x = 1;
        if (player.x > 83 - PWIDTH) player.x = 83 - PWIDTH;
      }

      if (y_axis != 0)
      {
        player.y += 12 * y_axis * DT;
        if (player.y < 9) player.y = 9;
        if (player.y > 47 - PHEIGHT) player.y = 47 - PHEIGHT;
      }

      char x2 = player.x + PWIDTH / 2 + 6 * cos(player_angle);
      char y2 = player.y + PHEIGHT / 2 + 6 * sin(player_angle);
      if (x2 < 1) x2 = 1;
      if (x2 > 83) x2 = 83;
      if (y2 < 9) y2 = 9;
      if (y2 > 47) y2 = 47;
      draw_line(player.x + PWIDTH / 2, player.y + PHEIGHT / 2, x2, y2);
      
      draw_sprite(&player);

      // missiles

      unsigned char fire_missile = 0;

      if (btn_right_pressed || usb_shoot)
      {
        btn_right_pressed = 0;
        fire_missile = 1;
      }

      for (unsigned char i = 0; i < NUM_MISSILES; i++)
      {
        if (missiles[i].is_visible)
        {
          missiles[i].x += missiles[i].dx * DT;
          missiles[i].y += missiles[i].dy * DT;
          draw_sprite(&missiles[i]);

          if (missiles[i].x < 1 || missiles[i].x > 83 - MWIDTH || missiles[i].y < 9 || missiles[i].y > 47 - MHEIGHT)
          {
            missiles[i].is_visible = 0;
            continue;
          }

          if (mothership_battle)
          {
            if (!(missiles[i].x >= mothership.x + MSWIDTH || missiles[i].x + MWIDTH <= mothership.x ||
                  missiles[i].y >= mothership.y + MSHEIGHT || missiles[i].y + MHEIGHT <= mothership.y))
            {
              missiles[i].is_visible = 0;
              mother_health--;

              // SWITCH TO NORMAL ENEMY MODE

              if (mother_health <= 0)
              {
                send_debug_string("Player destroyed the mothership");
                mothership_battle = 0;
                score += 10;
                reset_enemies(1);
                mother_missile.is_visible = 0;
              }
            }
          }
          else
          {
            for (unsigned char j = 0; j < NUM_ENEMIES; j++)
            {
              if (!enemies[j].is_visible) continue;

              if (!(missiles[i].x >= enemies[j].x + EWIDTH || missiles[i].x + MWIDTH <= enemies[j].x ||
                    missiles[i].y >= enemies[j].y + EHEIGHT || missiles[i].y + MHEIGHT <= enemies[j].y))
              {
                missiles[i].is_visible = 0;
                enemies[j].is_visible = 0;
                enemies[j].dx = 0;
                enemies[j].dy = 0;
                enemies_alive--;
                score++;
                send_debug_string("Player killed an alien");

                // SWITCH TO MOTHERSHIP BATTLE

                if (enemies_alive <= 0)
                {
                  mothership_battle = 1;
                  mothership.is_visible = 1;
                  mothership.dx = 0;
                  mothership.dy = 0;
                  mother_move_timer = 2 + 2 * (((float) rand()) / RAND_MAX);
                  mother_shoot_timer = 2 + 2 * (((float) rand()) / RAND_MAX);
                  mother_health = MOTHER_MAX_HEALTH;

                  unsigned char okay = 1;
                  unsigned char x;
                  unsigned char y;

                  while (1)
                  {
                    x = 1 + (82 - MSWIDTH) * (((float) rand()) / RAND_MAX);
                    y = 9 + (38 - MSHEIGHT) * (((float) rand()) / RAND_MAX);

                    if (!(x >= player.x + PWIDTH + 2 || x + MSWIDTH <= player.x - 2 || y >= player.y + PHEIGHT + 2 || y + MSHEIGHT <= player.y - 2))
                    {
                      okay = 0;
                    }

                    if (okay) break;
                  }

                  mothership.x = x;
                  mothership.y = y;
                }
              }
            }
          }
        }
        else if (fire_missile)
        {
          missiles[i].x = player.x + PWIDTH / 2 + 2 * cos(player_angle);
          missiles[i].y = player.y + PHEIGHT / 2 + 2 * sin(player_angle);
          missiles[i].dx = 10 * cos(player_angle);
          missiles[i].dy = 10 * sin(player_angle);
          missiles[i].is_visible = 1;
          fire_missile = 0;
        }
      }

      // border/status

      draw_border();
      draw_status();
    }
    else if (GAME_STATE == 3)
    {
      draw_string(19, 8, "GAME OVER");
      draw_string(0, 20, "Would you like");
      draw_string(0, 28, "to play again?");
      draw_string(0, 38, "Press a button...");

      if (btn_states[BTN_LEFT] == BTN_STATE_DOWN || btn_states[BTN_RIGHT] == BTN_STATE_DOWN)
      {
        GAME_STATE = 0;

        // ensures the game doesn't instantly start from the intro screen
        input_timer = 0.5;
      }
    }

    show_screen();

    if (GAME_STATE == 1)
    {
      _delay_ms(300);
    }
    else
    {
      _delay_ms(10);
    }
  }

  return 0;
}

ISR(TIMER0_COMPA_vect)
{
  for (unsigned char i = 0; i < NUM_BUTTONS; i++)
  {
    unsigned char state = 0;

    switch (i)
    {
      case BTN_DPAD_LEFT:
        state = GET_BIT(PINB, 1);
        break;
      case BTN_DPAD_RIGHT:
        state = GET_BIT(PIND, 0);
        break;
      case BTN_DPAD_UP:
        state = GET_BIT(PIND, 1);
        break;
      case BTN_DPAD_DOWN:
        state = GET_BIT(PINB, 7);
        break;
      case BTN_DPAD_CENTER:
        state = GET_BIT(PINB, 0);
        break;
      case BTN_LEFT:
        state = GET_BIT(PINF, 6);
        break;
      case BTN_RIGHT:
        state = GET_BIT(PINF, 5);
        break;
    }

    btn_hists[i] = btn_hists[i] << 1;
    SET_BIT(btn_hists[i], 0, state);

    if (btn_states[i] == BTN_STATE_DOWN && btn_hists[i] == 0x00)
    {
      btn_states[i] = BTN_STATE_UP;
    }
    else if (btn_states[i] == BTN_STATE_UP && btn_hists[i] == 0xFF)
    {
      btn_states[i] = BTN_STATE_DOWN;
      if (i == BTN_RIGHT) btn_right_pressed = 1;
    }
  }
}

ISR(TIMER1_OVF_vect)
{
  clock_overflow++;
}
