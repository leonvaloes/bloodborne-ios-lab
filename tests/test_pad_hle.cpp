#include "check.h"
#include "pad_hle.h"
#include "user_service.h"
#include "input.h"
#include "guest_memory.h"
#include <cstring>
#include <cmath>

static void test_user_service_lifecycle() {
    using namespace bb;
    UserServiceManager user_service;

    CHECK(!user_service.is_initialized());

    // Consultas antes de inicializar devem falhar com SCE_USER_SERVICE_ERROR_NOT_INITIALIZED
    SceUserServiceUserId uid = 0;
    CHECK(user_service.sceUserServiceGetInitialUser(&uid) == SCE_USER_SERVICE_ERROR_NOT_INITIALIZED);

    SceUserServiceLoginUserIdList list{};
    CHECK(user_service.sceUserServiceGetLoginIdList(&list) == SCE_USER_SERVICE_ERROR_NOT_INITIALIZED);

    char name_buf[32]{};
    CHECK(user_service.sceUserServiceGetUserName(SCE_USER_SERVICE_INITIAL_USER_ID, name_buf, sizeof(name_buf)) == SCE_USER_SERVICE_ERROR_NOT_INITIALIZED);

    // Inicialização do serviço
    CHECK(user_service.sceUserServiceInitialize(nullptr) == 0);
    CHECK(user_service.is_initialized());

    // Nova inicialização deve retornar erro ALREADY_INITIALIZED
    CHECK(user_service.sceUserServiceInitialize(nullptr) == SCE_USER_SERVICE_ERROR_ALREADY_INITIALIZED);

    // Encerramento
    CHECK(user_service.sceUserServiceTerminate() == 0);
    CHECK(!user_service.is_initialized());

    // Pós encerramento, consultas falham novamente
    CHECK(user_service.sceUserServiceGetInitialUser(&uid) == SCE_USER_SERVICE_ERROR_NOT_INITIALIZED);
}

static void test_user_service_queries() {
    using namespace bb;
    UserServiceManager user_service;
    CHECK(user_service.sceUserServiceInitialize(nullptr) == 0);

    // sceUserServiceGetInitialUser com ponteiro nulo
    CHECK(user_service.sceUserServiceGetInitialUser(static_cast<SceUserServiceUserId*>(nullptr)) == SCE_USER_SERVICE_ERROR_INVALID_ARG);

    // Consulta do usuário inicial
    SceUserServiceUserId uid = 0;
    CHECK(user_service.sceUserServiceGetInitialUser(&uid) == 0);
    CHECK(uid == SCE_USER_SERVICE_INITIAL_USER_ID);

    // Consulta em GuestMemory
    GuestMemory mem(256);
    CHECK(mem.map(0x1000, 128, read | write) == MemoryError::ok);

    CHECK(user_service.sceUserServiceGetInitialUser(mem, 0x1000) == 0);
    SceUserServiceUserId guest_uid = 0;
    CHECK(mem.load(0x1000, &guest_uid, sizeof(guest_uid)) == MemoryError::ok);
    CHECK(guest_uid == SCE_USER_SERVICE_INITIAL_USER_ID);

    // Endereço nulo ou não mapeado
    CHECK(user_service.sceUserServiceGetInitialUser(mem, 0) == SCE_USER_SERVICE_ERROR_INVALID_ARG);
    CHECK(user_service.sceUserServiceGetInitialUser(mem, 0x9000) == SCE_USER_SERVICE_ERROR_INVALID_ARG);

    // sceUserServiceGetLoginIdList
    CHECK(user_service.sceUserServiceGetLoginIdList(static_cast<SceUserServiceLoginUserIdList*>(nullptr)) == SCE_USER_SERVICE_ERROR_INVALID_ARG);

    SceUserServiceLoginUserIdList list{};
    CHECK(user_service.sceUserServiceGetLoginIdList(&list) == 0);
    CHECK(list.user_id[0] == SCE_USER_SERVICE_INITIAL_USER_ID);
    CHECK(list.user_id[1] == SCE_USER_SERVICE_USER_ID_INVALID);
    CHECK(list.user_id[2] == SCE_USER_SERVICE_USER_ID_INVALID);
    CHECK(list.user_id[3] == SCE_USER_SERVICE_USER_ID_INVALID);

    // Login list na memória guest
    CHECK(user_service.sceUserServiceGetLoginIdList(mem, 0x1020) == 0);
    SceUserServiceLoginUserIdList guest_list{};
    CHECK(mem.load(0x1020, &guest_list, sizeof(guest_list)) == MemoryError::ok);
    CHECK(guest_list.user_id[0] == SCE_USER_SERVICE_INITIAL_USER_ID);
    CHECK(guest_list.user_id[1] == SCE_USER_SERVICE_USER_ID_INVALID);

    // sceUserServiceGetUserName
    char name[32]{};
    CHECK(user_service.sceUserServiceGetUserName(uid, nullptr, 32) == SCE_USER_SERVICE_ERROR_INVALID_ARG);
    CHECK(user_service.sceUserServiceGetUserName(uid, name, 0) == SCE_USER_SERVICE_ERROR_INVALID_ARG);
    CHECK(user_service.sceUserServiceGetUserName(99999, name, sizeof(name)) == SCE_USER_SERVICE_ERROR_NO_USER);

    // Buffer menor que strlen("Hunter") + 1 (7 bytes)
    CHECK(user_service.sceUserServiceGetUserName(uid, name, 4) == SCE_USER_SERVICE_ERROR_BUFFER_TOO_SMALL);

    // Buffer correto
    CHECK(user_service.sceUserServiceGetUserName(uid, name, sizeof(name)) == 0);
    CHECK(std::strcmp(name, "Hunter") == 0);

    // Nome na memória guest
    CHECK(user_service.sceUserServiceGetUserName(mem, uid, 0x1040, 32) == 0);
    char guest_name[32]{};
    CHECK(mem.load(0x1040, guest_name, sizeof(guest_name)) == MemoryError::ok);
    CHECK(std::strcmp(guest_name, "Hunter") == 0);

    // Adição de segundo usuário e login
    const SceUserServiceUserId second_uid = 0x10000001;
    CHECK(user_service.login_user(second_uid, "Eileen"));
    CHECK(user_service.is_user_logged_in(second_uid));

    CHECK(user_service.sceUserServiceGetLoginIdList(&list) == 0);
    CHECK(list.user_id[0] == SCE_USER_SERVICE_INITIAL_USER_ID);
    CHECK(list.user_id[1] == second_uid);

    std::memset(name, 0, sizeof(name));
    CHECK(user_service.sceUserServiceGetUserName(second_uid, name, sizeof(name)) == 0);
    CHECK(std::strcmp(name, "Eileen") == 0);

    // Logout de usuário
    CHECK(user_service.logout_user(second_uid));
    CHECK(!user_service.is_user_logged_in(second_uid));
    CHECK(user_service.sceUserServiceGetUserName(second_uid, name, sizeof(name)) == SCE_USER_SERVICE_ERROR_NO_USER);
}

static void test_pad_lifecycle() {
    using namespace bb;
    UserServiceManager user_service;
    CHECK(user_service.sceUserServiceInitialize(nullptr) == 0);

    InputManager input;
    PadHleManager pad_mgr(&input, &user_service);

    CHECK(pad_mgr.active_pad_count() == 0);

    // Validações estritas de parâmetros no scePadOpen
    // userId inválido
    CHECK(pad_mgr.scePadOpen(0, SCE_PAD_PORT_TYPE_STANDARD, 0) == SCE_PAD_ERROR_INVALID_ARG);
    CHECK(pad_mgr.scePadOpen(static_cast<std::uint32_t>(SCE_USER_SERVICE_USER_ID_INVALID), SCE_PAD_PORT_TYPE_STANDARD, 0) == SCE_PAD_ERROR_INVALID_ARG);
    // userId não logado no UserService
    CHECK(pad_mgr.scePadOpen(0x20000000, SCE_PAD_PORT_TYPE_STANDARD, 0) == SCE_PAD_ERROR_INVALID_ARG);
    // type inválido
    CHECK(pad_mgr.scePadOpen(SCE_USER_SERVICE_INITIAL_USER_ID, 99, 0) == SCE_PAD_ERROR_INVALID_ARG);
    // index fora de bounds
    CHECK(pad_mgr.scePadOpen(SCE_USER_SERVICE_INITIAL_USER_ID, SCE_PAD_PORT_TYPE_STANDARD, -1) == SCE_PAD_ERROR_INVALID_ARG);
    CHECK(pad_mgr.scePadOpen(SCE_USER_SERVICE_INITIAL_USER_ID, SCE_PAD_PORT_TYPE_STANDARD, 4) == SCE_PAD_ERROR_INVALID_ARG);

    // Abertura bem sucedida
    std::int32_t handle1 = pad_mgr.scePadOpen(SCE_USER_SERVICE_INITIAL_USER_ID, SCE_PAD_PORT_TYPE_STANDARD, 0);
    CHECK(handle1 > 0);
    CHECK(pad_mgr.is_handle_valid(handle1));
    CHECK(pad_mgr.active_pad_count() == 1);

    // Logar segundo usuário
    user_service.login_user(0x10000001, "LadyMaria");

    // Tentativa de abrir o mesmo index (0) já em uso
    CHECK(pad_mgr.scePadOpen(0x10000001, SCE_PAD_PORT_TYPE_STANDARD, 0) == SCE_PAD_ERROR_ALREADY_OPENED);
    // Tentativa de abrir o mesmo userId em outro index
    CHECK(pad_mgr.scePadOpen(SCE_USER_SERVICE_INITIAL_USER_ID, SCE_PAD_PORT_TYPE_STANDARD, 1) == SCE_PAD_ERROR_ALREADY_OPENED);

    // Abrir porta 1 para o segundo usuário
    std::int32_t handle2 = pad_mgr.scePadOpen(0x10000001, SCE_PAD_PORT_TYPE_STANDARD, 1);
    CHECK(handle2 > 0);
    CHECK(handle2 != handle1);
    CHECK(pad_mgr.active_pad_count() == 2);

    // Fechamento com handles inválidos
    CHECK(pad_mgr.scePadClose(-1) == SCE_PAD_ERROR_INVALID_HANDLE);
    CHECK(pad_mgr.scePadClose(0) == SCE_PAD_ERROR_INVALID_HANDLE);
    CHECK(pad_mgr.scePadClose(9999) == SCE_PAD_ERROR_INVALID_HANDLE);

    // Fechamento válido
    CHECK(pad_mgr.scePadClose(handle1) == 0);
    CHECK(!pad_mgr.is_handle_valid(handle1));
    CHECK(pad_mgr.active_pad_count() == 1);

    // Fechar novamente handle já fechado
    CHECK(pad_mgr.scePadClose(handle1) == SCE_PAD_ERROR_INVALID_HANDLE);

    // Reabrir porta 0 para o usuário inicial
    std::int32_t handle1_new = pad_mgr.scePadOpen(SCE_USER_SERVICE_INITIAL_USER_ID, SCE_PAD_PORT_TYPE_STANDARD, 0);
    CHECK(handle1_new > 0);
    CHECK(pad_mgr.active_pad_count() == 2);

    // Fechar todos
    CHECK(pad_mgr.scePadClose(handle1_new) == 0);
    CHECK(pad_mgr.scePadClose(handle2) == 0);
    CHECK(pad_mgr.active_pad_count() == 0);
}

static void test_pad_read_state_guest_memory() {
    using namespace bb;
    InputManager input;
    UserServiceManager user_service;
    CHECK(user_service.sceUserServiceInitialize(nullptr) == 0);

    PadHleManager pad_mgr(&input, &user_service);
    std::int32_t handle = pad_mgr.scePadOpen(SCE_USER_SERVICE_INITIAL_USER_ID, SCE_PAD_PORT_TYPE_STANDARD, 0);
    CHECK(handle > 0);

    // Configurar estado no InputManager
    input.reset();
    input.set_button(PAD_CROSS, true);
    input.set_button(PAD_R1, true);
    input.set_button(PAD_OPTIONS, true);
    input.set_left_stick(0.5f, 0.0f);
    input.set_right_stick(-1.0f, 0.0f);
    input.set_triggers(0.80f, 0.15f);
    input.set_connected(true);

    // Leitura em buffer host
    ScePadData host_data{};
    CHECK(pad_mgr.scePadReadState(handle, &host_data) == 0);

    CHECK((host_data.buttons & PAD_CROSS) != 0);
    CHECK((host_data.buttons & PAD_R1) != 0);
    CHECK((host_data.buttons & PAD_OPTIONS) != 0);
    CHECK((host_data.buttons & PAD_CIRCLE) == 0);

    // Analógicos calibrados
    // 0.5f com deadzone 0.15 escala para 0.4117 -> uint8 180, neutro Y -> 128
    CHECK(host_data.left_stick_x >= 175 && host_data.left_stick_x <= 185);
    CHECK(host_data.left_stick_y == 128);
    CHECK(host_data.right_stick_x == 0);
    CHECK(host_data.right_stick_y == 128);

    // Gatilhos
    CHECK(host_data.l2_analog >= 200 && host_data.l2_analog <= 210); // 0.80 * 255 = 204
    CHECK(host_data.r2_analog >= 30 && host_data.r2_analog <= 45);   // 0.15 * 255 = 38
    CHECK(host_data.connected == 1);
    CHECK(host_data.timestamp > 0);

    // Leitura em GuestMemory
    GuestMemory mem(256);
    CHECK(mem.map(0x4000, 128, read | write) == MemoryError::ok);

    CHECK(pad_mgr.scePadReadState(mem, handle, 0x4000) == 0);

    ScePadData guest_data{};
    CHECK(mem.load(0x4000, &guest_data, sizeof(guest_data)) == MemoryError::ok);

    CHECK(guest_data.buttons == host_data.buttons);
    CHECK(guest_data.left_stick_x == host_data.left_stick_x);
    CHECK(guest_data.left_stick_y == host_data.left_stick_y);
    CHECK(guest_data.right_stick_x == host_data.right_stick_x);
    CHECK(guest_data.right_stick_y == host_data.right_stick_y);
    CHECK(guest_data.l2_analog == host_data.l2_analog);
    CHECK(guest_data.r2_analog == host_data.r2_analog);
    CHECK(guest_data.connected == 1);
    CHECK(guest_data.timestamp >= host_data.timestamp);

    // Validações estritas de bounds e ponteiros
    CHECK(pad_mgr.scePadReadState(handle, static_cast<ScePadData*>(nullptr)) == SCE_PAD_ERROR_INVALID_ARG);
    CHECK(pad_mgr.scePadReadState(-1, &host_data) == SCE_PAD_ERROR_INVALID_HANDLE);
    CHECK(pad_mgr.scePadReadState(mem, handle, 0) == SCE_PAD_ERROR_INVALID_ARG);
    CHECK(pad_mgr.scePadReadState(mem, handle, 0x9000) == SCE_PAD_ERROR_INVALID_ARG); // não mapeado

    // Página sem permissão de escrita
    CHECK(mem.map(0x6000, 64, read) == MemoryError::ok);
    CHECK(pad_mgr.scePadReadState(mem, handle, 0x6000) == SCE_PAD_ERROR_INVALID_ARG);

    // Teste de desconexão
    input.set_connected(false);
    CHECK(pad_mgr.scePadReadState(mem, handle, 0x4000) == 0);
    CHECK(mem.load(0x4000, &guest_data, sizeof(guest_data)) == MemoryError::ok);
    CHECK(guest_data.connected == 0);
}

static void test_pad_vibration_and_lightbar() {
    using namespace bb;
    UserServiceManager user_service;
    CHECK(user_service.sceUserServiceInitialize(nullptr) == 0);

    InputManager input;
    PadHleManager pad_mgr(&input, &user_service);
    std::int32_t handle = pad_mgr.scePadOpen(SCE_USER_SERVICE_INITIAL_USER_ID, SCE_PAD_PORT_TYPE_STANDARD, 0);
    CHECK(handle > 0);

    // Vibração via struct host
    CHECK(pad_mgr.scePadSetVibration(handle, nullptr) == SCE_PAD_ERROR_INVALID_ARG);
    CHECK(pad_mgr.scePadSetVibration(-1, nullptr) == SCE_PAD_ERROR_INVALID_ARG);

    ScePadVibrationParam vib{175, 85};
    CHECK(pad_mgr.scePadSetVibration(handle, &vib) == 0);
    auto cur_vib = pad_mgr.get_vibration(handle);
    CHECK(cur_vib.large_motor == 175);
    CHECK(cur_vib.small_motor == 85);

    // Vibração via GuestMemory
    GuestMemory mem(256);
    CHECK(mem.map(0x5000, 128, read | write) == MemoryError::ok);

    ScePadVibrationParam guest_vib_in{240, 120};
    CHECK(mem.store(0x5000, &guest_vib_in, sizeof(guest_vib_in)) == MemoryError::ok);
    CHECK(pad_mgr.scePadSetVibration(mem, handle, 0x5000) == 0);
    cur_vib = pad_mgr.get_vibration(handle);
    CHECK(cur_vib.large_motor == 240);
    CHECK(cur_vib.small_motor == 120);

    // Endereço nulo ou inválido
    CHECK(pad_mgr.scePadSetVibration(mem, handle, 0) == SCE_PAD_ERROR_INVALID_ARG);
    CHECK(pad_mgr.scePadSetVibration(mem, handle, 0x8888) == SCE_PAD_ERROR_INVALID_ARG);

    // LightBar via struct host
    CHECK(pad_mgr.scePadSetLightBar(handle, nullptr) == SCE_PAD_ERROR_INVALID_ARG);

    ScePadColor blood_red{220, 20, 60, 0};
    CHECK(pad_mgr.scePadSetLightBar(handle, &blood_red) == 0);
    auto cur_color = pad_mgr.get_lightbar(handle);
    CHECK(cur_color.r == 220);
    CHECK(cur_color.g == 20);
    CHECK(cur_color.b == 60);

    // LightBar via GuestMemory
    ScePadColor hunter_green{34, 139, 34, 0};
    CHECK(mem.store(0x5010, &hunter_green, sizeof(hunter_green)) == MemoryError::ok);
    CHECK(pad_mgr.scePadSetLightBar(mem, handle, 0x5010) == 0);
    cur_color = pad_mgr.get_lightbar(handle);
    CHECK(cur_color.r == 34);
    CHECK(cur_color.g == 139);
    CHECK(cur_color.b == 34);

    // Endereço nulo ou inválido
    CHECK(pad_mgr.scePadSetLightBar(mem, handle, 0) == SCE_PAD_ERROR_INVALID_ARG);
    CHECK(pad_mgr.scePadSetLightBar(mem, handle, 0x9999) == SCE_PAD_ERROR_INVALID_ARG);
}

static void test_pad_deadzone_and_calibration() {
    using namespace bb;
    InputManager input;
    UserServiceManager user_service;
    CHECK(user_service.sceUserServiceInitialize(nullptr) == 0);

    PadHleManager pad_mgr(&input, &user_service);
    std::int32_t handle = pad_mgr.scePadOpen(SCE_USER_SERVICE_INITIAL_USER_ID, SCE_PAD_PORT_TYPE_STANDARD, 0);
    CHECK(handle > 0);

    // Centro neutro estrito
    input.reset();
    input.set_left_stick(0.0f, 0.0f);
    input.set_right_stick(0.0f, 0.0f);

    ScePadData data{};
    CHECK(pad_mgr.scePadReadState(handle, &data) == 0);
    CHECK(data.left_stick_x == 128);
    CHECK(data.left_stick_y == 128);
    CHECK(data.right_stick_x == 128);
    CHECK(data.right_stick_y == 128);

    // Stick drift dentro da zona morta (deadzone = 0.20f)
    pad_mgr.set_deadzone(handle, 0.20f);
    input.set_left_stick(0.08f, -0.08f);
    input.set_right_stick(-0.12f, 0.05f);

    CHECK(pad_mgr.scePadReadState(handle, &data) == 0);
    CHECK(data.left_stick_x == 128);
    CHECK(data.left_stick_y == 128);
    CHECK(data.right_stick_x == 128);
    CHECK(data.right_stick_y == 128);

    // Deflexão total além da zona morta
    input.set_left_stick(1.0f, 0.0f);
    CHECK(pad_mgr.scePadReadState(handle, &data) == 0);
    CHECK(data.left_stick_x == 255);
    CHECK(data.left_stick_y == 128);

    input.set_left_stick(0.0f, -1.0f);
    CHECK(pad_mgr.scePadReadState(handle, &data) == 0);
    CHECK(data.left_stick_x == 128);
    CHECK(data.left_stick_y == 0);
}

static void test_global_hle_wrappers() {
    using namespace bb;
    // Teste dos wrappers globais que usam instâncias singleton padrão
    default_user_service().reset();
    default_pad_hle().reset();

    CHECK(sceUserServiceInitialize(nullptr) == 0);

    SceUserServiceUserId uid = 0;
    CHECK(sceUserServiceGetInitialUser(&uid) == 0);
    CHECK(uid == SCE_USER_SERVICE_INITIAL_USER_ID);

    char name[32]{};
    CHECK(sceUserServiceGetUserName(uid, name, sizeof(name)) == 0);
    CHECK(std::strcmp(name, "Hunter") == 0);

    std::int32_t handle = scePadOpen(uid, SCE_PAD_PORT_TYPE_STANDARD, 0);
    CHECK(handle > 0);

    ScePadData pad_data{};
    CHECK(scePadReadState(handle, &pad_data) == 0);
    CHECK(pad_data.connected == 1);

    ScePadVibrationParam vib{50, 25};
    CHECK(scePadSetVibration(handle, &vib) == 0);

    ScePadColor color{10, 20, 30, 0};
    CHECK(scePadSetLightBar(handle, &color) == 0);

    CHECK(scePadClose(handle) == 0);
    CHECK(sceUserServiceTerminate() == 0);
}

int main() {
    return run([] {
        test_user_service_lifecycle();
        test_user_service_queries();
        test_pad_lifecycle();
        test_pad_read_state_guest_memory();
        test_pad_vibration_and_lightbar();
        test_pad_deadzone_and_calibration();
        test_global_hle_wrappers();
    });
}
