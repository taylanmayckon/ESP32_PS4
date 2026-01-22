#include <string.h>
#include <freertos/FreeRTOS.h>
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include <uni.h>
#include <esp_log.h>
#include <esp_err.h>
#include "driver/ledc.h"
#include <math.h>
#include <stdio.h>

/////////////
// ===== DEFINIÇÃO DE PINOS – BTS7960 =====

// MOTOR ESQUERDO
#define ESQ_RPWM GPIO_NUM_18
#define ESQ_LPWM GPIO_NUM_19
#define ESQ_REN  GPIO_NUM_4
#define ESQ_LEN  GPIO_NUM_5

// MOTOR DIREITO
#define DIR_RPWM GPIO_NUM_21
#define DIR_LPWM GPIO_NUM_22
#define DIR_REN  GPIO_NUM_23
#define DIR_LEN  GPIO_NUM_25

// ===== PWM =====
#define LEDC_TIMER        LEDC_TIMER_0
#define LEDC_MODE         LEDC_HIGH_SPEED_MODE
#define LEDC_DUTY_RES     LEDC_TIMER_13_BIT
#define TOP_PWM           8191
#define LEDC_FREQUENCY    4000

#define LEDC_CH_ESQ_R     LEDC_CHANNEL_0
#define LEDC_CH_ESQ_L     LEDC_CHANNEL_1
#define LEDC_CH_DIR_R     LEDC_CHANNEL_2
#define LEDC_CH_DIR_L     LEDC_CHANNEL_3

#define AXIS_MAX 512.0
////////

// Isso aqui só mantive do exemplo
// Custom "instance"
typedef struct my_platform_instance_s {
    uni_gamepad_seat_t gamepad_seat;  // which "seat" is being used
} my_platform_instance_t;

// Declarations (Só mantive do exemplo)
static void trigger_event_on_gamepad(uni_hid_device_t* d);
static my_platform_instance_t* get_my_platform_instance(uni_hid_device_t* d);

// Variáveis globais
int left_x, left_y, right_x, right_y;

// Funcao de deadzone para o controle
int apply_deadzone(int value, int deadzone) {
    if((value > -deadzone) && (value < deadzone)){
        value = 0;
    }
    return value;
}

// ===== TASK DE CONTROLE DOS MOTORES =====
void PWMConfigurationAndValueUpdate(void *pvParameter) {

    // Timer PWM
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Configuração dos 4 canais PWM
    ledc_channel_config_t channels[] = {
        {
            .gpio_num = ESQ_RPWM,
            .speed_mode = LEDC_MODE,
            .channel = LEDC_CH_ESQ_R,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER,
            .duty = 0,
            .hpoint = 0
        },
        {
            .gpio_num = ESQ_LPWM,
            .speed_mode = LEDC_MODE,
            .channel = LEDC_CH_ESQ_L,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER,
            .duty = 0,
            .hpoint = 0
        },
        {
            .gpio_num = DIR_RPWM,
            .speed_mode = LEDC_MODE,
            .channel = LEDC_CH_DIR_R,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER,
            .duty = 0,
            .hpoint = 0
        },
        {
            .gpio_num = DIR_LPWM,
            .speed_mode = LEDC_MODE,
            .channel = LEDC_CH_DIR_L,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER,
            .duty = 0,
            .hpoint = 0
        }
    };

    for (int i = 0; i < 4; i++)
        ESP_ERROR_CHECK(ledc_channel_config(&channels[i]));

    // ENABLES, sempre ativados, cada enable ativa um lado da ponte H
    gpio_set_direction(ESQ_REN, GPIO_MODE_OUTPUT);
    gpio_set_direction(ESQ_LEN, GPIO_MODE_OUTPUT);
    gpio_set_direction(DIR_REN, GPIO_MODE_OUTPUT);
    gpio_set_direction(DIR_LEN, GPIO_MODE_OUTPUT);

    gpio_set_level(ESQ_REN, 1);
    gpio_set_level(ESQ_LEN, 1);
    gpio_set_level(DIR_REN, 1);
    gpio_set_level(DIR_LEN, 1);

    // Variáveis que guardam a direção atual e última penúltima direção
    bool dir_esq = 1, dir_dir = 1, bool prev_dir_esq = 1, prev_dir_dir = 1;

    // Valor de tolerância, se o joystick mandar um sinal menor que esse nada acontece, isso evita que o robô se mova erraticamente por conta de ruído
    int deadzone = 40;

    while (1) {
        // Se o valor estiver entre -deadzone e +deadzone, trata como se o controle estivesse exatamente no centro = 0
        float x_axis = apply_deadzone(left_x, deadzone);
        float y_axis = apply_deadzone(left_y, deadzone);

        // K controla o quão suave o robô faz curvas
        float k = 5.0;
        // Variável curva pode ser negativa ou positiva dependendo de x_axis pois: -512 < x_axis < 512
        float curva = x_axis / k;

        // Caso valor de curva seja positivo, motor esquerdo gira mais rápido que o direito, curva pra direita
        // Caso valor de curva seja negativo, motor direito gira mais rápido que o esquerdo, curva pra esquerda
        float m_esq = y_axis + curva;
        float m_dir = y_axis - curva;

        // Garante que os valores não passem do máximo permitido
        if (m_esq > AXIS_MAX) m_esq = AXIS_MAX;
        if (m_esq < -AXIS_MAX) m_esq = -AXIS_MAX;
        if (m_dir > AXIS_MAX) m_dir = AXIS_MAX;
        if (m_dir < -AXIS_MAX) m_dir = -AXIS_MAX;

        // Cálculo do PWM de cada um dos motores, note que um único sinal PWM define direção(- ou +) e intensidade(valor do PWM) pra cada motor
        uint32_t pwm_esq = (uint32_t)((fabs(m_esq) / AXIS_MAX) * TOP_PWM);
        uint32_t pwm_dir = (uint32_t)((fabs(m_dir) / AXIS_MAX) * TOP_PWM);

        // Verifica se a direção atual é pra frente ou pra trás
        dir_esq = (m_esq >= 0);
        dir_dir = (m_dir >= 0);

        // Proteção contra inversão brusca
        // Compara a direção atual com a penúltima, caso sejam diferentes, zera o PWM antes de setar a nova direção
        // Isso evita correntes em direções indesejadas, equivalente a desligar antes de tentar ligar de novo
        if (dir_esq != prev_dir_esq || dir_dir != prev_dir_dir) {
            ledc_set_duty(LEDC_MODE, LEDC_CH_ESQ_R, 0);
            ledc_set_duty(LEDC_MODE, LEDC_CH_ESQ_L, 0);
            ledc_set_duty(LEDC_MODE, LEDC_CH_DIR_R, 0);
            ledc_set_duty(LEDC_MODE, LEDC_CH_DIR_L, 0);

            ledc_update_duty(LEDC_MODE, LEDC_CH_ESQ_R);
            ledc_update_duty(LEDC_MODE, LEDC_CH_ESQ_L);
            ledc_update_duty(LEDC_MODE, LEDC_CH_DIR_R);
            ledc_update_duty(LEDC_MODE, LEDC_CH_DIR_L);

            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Salvando a direção atual, pois abaixo atualizaremos a direção com base no controle e a direção atual vai virar a penúltima
        prev_dir_esq = dir_esq;
        prev_dir_dir = dir_dir;

        // ===== MOTOR ESQUERDO =====
        if (dir_esq) {
            // A ordem em que setamos o PWM é sempre: desativar um lado e só depois ativar o outro
            // Isso evita que dois lados possam estar ligados ao mesmo tempo
            ledc_set_duty(LEDC_MODE, LEDC_CH_ESQ_L, 0);
            ledc_set_duty(LEDC_MODE, LEDC_CH_ESQ_R, pwm_esq);
            ledc_update_duty(LEDC_MODE, LEDC_CH_ESQ_L);
            ledc_update_duty(LEDC_MODE, LEDC_CH_ESQ_R);
        } else {
            ledc_set_duty(LEDC_MODE, LEDC_CH_ESQ_R, 0);
            ledc_set_duty(LEDC_MODE, LEDC_CH_ESQ_L, pwm_esq);
            ledc_update_duty(LEDC_MODE, LEDC_CH_ESQ_R);
            ledc_update_duty(LEDC_MODE, LEDC_CH_ESQ_L);
        }

        // ===== MOTOR DIREITO =====
        if (dir_dir) {
            // A ordem em que setamos o PWM é sempre: desativar um lado e só depois ativar o outro
            // Isso evita que dois lados possam estar ligados ao mesmo tempo
            ledc_set_duty(LEDC_MODE, LEDC_CH_DIR_L, 0);
            ledc_set_duty(LEDC_MODE, LEDC_CH_DIR_R, pwm_dir);
            ledc_update_duty(LEDC_MODE, LEDC_CH_DIR_L);
            ledc_update_duty(LEDC_MODE, LEDC_CH_DIR_R);
        } else {
            ledc_set_duty(LEDC_MODE, LEDC_CH_DIR_R, 0);
            ledc_set_duty(LEDC_MODE, LEDC_CH_DIR_L, pwm_dir);
            ledc_update_duty(LEDC_MODE, LEDC_CH_DIR_R);
            ledc_update_duty(LEDC_MODE, LEDC_CH_DIR_L);
        }

        // Delay por segurança, mas uma vez pra evitar correntes inprevisíveis caso as direções mudem rapidammente
        // Opcional no uso do driver, e extra, pois já temos verificações acima
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void TaskPrintAxis() {
    while(1){
        logi("ESQUERDO-> X: %d | Y: %d\n", left_x, left_y);
        logi("DIREITO-> X: %d | Y: %d\n\n\n", right_x, right_y);
        vTaskDelay(700/portTICK_PERIOD_MS);
    }
}

// É nessa função que vão as configurações, FreeRTOS e afins
static void my_platform_init(int argc, const char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    logi("Bluepad32: Inicializando configuração\n");

    // Criando a task que vai fazer o print dos valores dos eixos
    xTaskCreate(&PWMConfigurationAndValueUpdate, "PWM configuration", 4096, NULL, 1, NULL);
    xTaskCreate(&TaskPrintAxis, "printAxis", 4096, NULL, 5, NULL);
}

static void my_platform_on_init_complete(void) {
    logi("Bluepad32: Configuração concluída\n");

    // Inicia a busca por dispositivos
    uni_bt_start_scanning_and_autoconnect_unsafe();
    uni_bt_allow_incoming_connections(true);

    // Baseada no tempo de execução, isso deleta ou lista as chaves Bluetooth armazenadas
    if (1)
        uni_bt_del_keys_unsafe();
    else
        uni_bt_list_keys_unsafe();
}

static uni_error_t my_platform_on_device_discovered(bd_addr_t addr, const char* name, uint16_t cod, uint8_t rssi) {
    // You can filter discovered devices here.
    // Just return any value different from UNI_ERROR_SUCCESS;
    // @param addr: the Bluetooth address
    // @param name: could be NULL, could be zero-length, or might contain the name.
    // @param cod: Class of Device. See "uni_bt_defines.h" for possible values.
    // @param rssi: Received Signal Strength Indicator (RSSI) measured in dBms. The higher (255) the better.

    // As an example, if you want to filter out keyboards, do:
    if (((cod & UNI_BT_COD_MINOR_MASK) & UNI_BT_COD_MINOR_KEYBOARD) == UNI_BT_COD_MINOR_KEYBOARD) {
        logi("Ignoring keyboard\n");
        return UNI_ERROR_IGNORE_DEVICE;
    }

    return UNI_ERROR_SUCCESS;
}

static void my_platform_on_device_connected(uni_hid_device_t* d) {
    logi("Bluepad32: Dispositivo conectado: %p\n", d);
}

static void my_platform_on_device_disconnected(uni_hid_device_t* d) {
    logi("Bluepad32: Dispositivo desconectado: %p\n", d);
}

static uni_error_t my_platform_on_device_ready(uni_hid_device_t* d) {
    logi("Bluepad32: Dispositivo pronto: %p\n", d);
    my_platform_instance_t* ins = get_my_platform_instance(d);
    ins->gamepad_seat = GAMEPAD_SEAT_A;

    trigger_event_on_gamepad(d);
    return UNI_ERROR_SUCCESS;
}

static void my_platform_on_controller_data(uni_hid_device_t* d, uni_controller_t* ctl) {
    static uni_controller_t prev = {0};
    
    // Variável para armazenar as informações recebidas do controle
    // Basicamente vai em:
    // componentes/bluepad32/include/controller/uni_gamepad.h
    // Lá tem todas as structs de valores que você pode receber
    uni_gamepad_t* gp;
    uni_gamepad_mappings_t* tg;

    // Comenta isso aqui no projeto final, no exemplo diz que é para não ter
    // spam desnecessário de logs
    if (memcmp(&prev, ctl, sizeof(*ctl)) == 0) {
        return;
    }
    prev = *ctl;

    // Essa função é para garantir que só vai executar ações caso
    // o que tenha se conectado seja um controle do tipo GAMEPAD
    // no caso o controle de PS4, se for testar algo diferente, coemnta o if
    if(ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD) {
        return;
    }

    // Mudando a cor do led do controle para verde (8 bits)
    //                                     r,  g,  0
    d->report_parser.set_lightbar_color(d, 0, 255, 0);
    
    // Extrai os dados do controle
    gp = &ctl->gamepad;

    // Valores dos analógicos
    left_x = gp->axis_x;
    left_y = gp->axis_y;
    right_x = gp->axis_rx;
    right_y = gp->axis_ry;
}

static const uni_property_t* my_platform_get_property(uni_property_idx_t idx) {
    ARG_UNUSED(idx);
    return NULL;
}

static void my_platform_on_oob_event(uni_platform_oob_event_t event, void* data) {
    switch (event) {
        case UNI_PLATFORM_OOB_GAMEPAD_SYSTEM_BUTTON: {
            uni_hid_device_t* d = data;

            if (d == NULL) {
                loge("ERROR: my_platform_on_oob_event: Invalid NULL device\n");
                return;
            }
            logi("custom: on_device_oob_event(): %d\n", event);

            my_platform_instance_t* ins = get_my_platform_instance(d);
            ins->gamepad_seat = ins->gamepad_seat == GAMEPAD_SEAT_A ? GAMEPAD_SEAT_B : GAMEPAD_SEAT_A;

            trigger_event_on_gamepad(d);
            break;
        }

        case UNI_PLATFORM_OOB_BLUETOOTH_ENABLED:
            logi("custom: Bluetooth enabled: %d\n", (bool)(data));
            break;

        default:
            logi("my_platform_on_oob_event: unsupported event: 0x%04x\n", event);
            break;
    }
}

//
// Helpers
//
static my_platform_instance_t* get_my_platform_instance(uni_hid_device_t* d) {
    return (my_platform_instance_t*)&d->platform_data[0];
}

static void trigger_event_on_gamepad(uni_hid_device_t* d) {
    my_platform_instance_t* ins = get_my_platform_instance(d);

    if (d->report_parser.play_dual_rumble != NULL) {
        d->report_parser.play_dual_rumble(d, 0 /* delayed start ms */, 150 /* duration ms */, 128 /* weak magnitude */,
                                          40 /* strong magnitude */);
    }

    if (d->report_parser.set_player_leds != NULL) {
        d->report_parser.set_player_leds(d, ins->gamepad_seat);
    }

    if (d->report_parser.set_lightbar_color != NULL) {
        uint8_t red = (ins->gamepad_seat & 0x01) ? 0xff : 0;
        uint8_t green = (ins->gamepad_seat & 0x02) ? 0xff : 0;
        uint8_t blue = (ins->gamepad_seat & 0x04) ? 0xff : 0;
        d->report_parser.set_lightbar_color(d, red, green, blue);
    }
}

//
// Entry Point
//
struct uni_platform* get_my_platform(void) {
    static struct uni_platform plat = {
        .name = "custom",
        .init = my_platform_init,
        .on_init_complete = my_platform_on_init_complete,
        .on_device_discovered = my_platform_on_device_discovered,
        .on_device_connected = my_platform_on_device_connected,
        .on_device_disconnected = my_platform_on_device_disconnected,
        .on_device_ready = my_platform_on_device_ready,
        .on_oob_event = my_platform_on_oob_event,
        .on_controller_data = my_platform_on_controller_data,
        .get_property = my_platform_get_property,
    };

    return &plat;
}
