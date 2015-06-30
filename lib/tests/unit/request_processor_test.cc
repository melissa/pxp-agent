#include "certs.hpp"

#include "root_path.hpp"

#include <cthun-agent/request_processor.hpp>
#include <cthun-agent/errors.hpp>
#include <cthun-agent/configuration.hpp>

#include <leatherman/json_container/json_container.hpp>

#include <catch.hpp>

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>

#include <memory>
#include <exception>
#include <unistd.h>
#include <atomic>

namespace CthunAgent {

namespace lth_jc = leatherman::json_container;

//
// Mocking class
//

class TestConnector : public CthunConnector {
  public:
    struct cthunError_msg : public std::exception {
        const char* what() const noexcept { return "cthun error"; } };
    struct rpcError_msg : public std::exception {
        const char* what() const noexcept { return "rpc error"; } };
    struct blocking_response : public std::exception {
        const char* what() const noexcept { return "blocking response"; } };

    std::atomic<bool> sent_provisional_response;
    std::atomic<bool> sent_non_blocking_response;


    TestConnector(const std::string& server_url,
                  const std::string& client_type,
                  const std::string& ca_crt_path,
                  const std::string& client_crt_path,
                  const std::string& client_key_path)
            : CthunConnector { server_url, client_type, ca_crt_path,
                               client_crt_path, client_key_path },
              sent_provisional_response { false },
              sent_non_blocking_response {false} {
    }

    void sendCthunError(const std::string&,
                        const std::string&,
                        const std::vector<std::string>&) {
        throw cthunError_msg {};
    }

    void sendRPCError(const ActionRequest&,
                      const std::string&) {
        throw rpcError_msg {};
    }

    void sendBlockingResponse(const ActionRequest&,
                              const LTH_JC::JsonContainer&) {
        throw blocking_response {};
    }

    // Don't throw for non-blocking transactions - will spawn
    // another thread

    virtual void sendNonBlockingResponse(const ActionRequest&,
                                         const LTH_JC::JsonContainer&,
                                         const std::string&) {
        sent_non_blocking_response = true;
    }

    virtual void sendProvisionalResponse(const ActionRequest&,
                                         const std::string&,
                                         const std::string&) {
        sent_provisional_response = true;
    }
};

//
// Tests
//

static const std::string TEST_SERVER_URL { "wss://127.0.0.1:8090/cthun/" };
static const std::string CA { getCaPath() };
static const std::string CERT { getCertPath() };
static const std::string KEY { getKeyPath() };
static const std::string MODULES { CTHUN_AGENT_ROOT_PATH
                            + std::string { "/lib/tests/resources/modules" } };
static const std::string SPOOL { CTHUN_AGENT_ROOT_PATH
                          + std::string { "/lib/tests/resources/tmp" } };

TEST_CASE("RequestProcessor::RequestProcessor", "[agent]") {
    auto c_ptr = std::make_shared<TestConnector>(TEST_SERVER_URL, "test_agent",
                                                 CA, CERT, KEY);

    SECTION("successfully instantiates with valid arguments") {
        auto m_path = CTHUN_AGENT_ROOT_PATH + std::string { "/fake_dir" };
        REQUIRE_NOTHROW(RequestProcessor(c_ptr, m_path));
    };
}

static std::string valid_envelope_txt {
    " { \"id\" : \"123456\","
    "   \"message_type\" : \"test_test_test\","
    "   \"expires\" : \"2015-06-26T22:57:09Z\","
    "   \"targets\" : [\"cth://agent/test_agent\"],"
    "   \"sender\" : \"cth://controller/test_controller\","
    "   \"destination_report\" : false"
    " }" };

static std::string rpc_data_txt {
    " { \"transaction_id\" : \"42\","
    "   \"module\" : \"test_module\","
    "   \"action\" : \"test_action\","
    "   \"params\" : { \"path\" : \"/tmp\","
    "                  \"urgent\" : true }"
    " }" };

static void configureTest() {
    const char* argv[] { "test-command",
                         "--server", TEST_SERVER_URL.data(),
                         "--ca", CA.data(),
                         "--cert", CERT.data(),
                         "--key", KEY.data(),
                         "--spool-dir", SPOOL.data() };
    int argc { 9 };
    Configuration::Instance().initialize(argc, const_cast<char**>(argv));
}

static void resetTest() {
    Configuration::Instance().reset();
}

TEST_CASE("RequestProcessor::processRequest", "[agent]") {
    resetTest();
    configureTest();

    auto c_ptr = std::make_shared<TestConnector>(TEST_SERVER_URL, "test_agent",
                                                 getCaPath(), getCertPath(),
                                                 getKeyPath());
    RequestProcessor r_p { c_ptr, MODULES };

    lth_jc::JsonContainer envelope { valid_envelope_txt };
    std::vector<lth_jc::JsonContainer> debug {};

    SECTION("reply with a Cthun error if request has bad format") {
        const CthunClient::ParsedChunks p_c { envelope, false, debug, 0 };

        REQUIRE_THROWS_AS(r_p.processRequest(RequestType::Blocking, p_c),
                          TestConnector::cthunError_msg);
    }

    lth_jc::JsonContainer data {};
    data.set<std::string>("transaction_id", "42");

    SECTION("reply with an RPC error if request has invalid content") {
        SECTION("uknown module") {
            data.set<std::string>("module", "foo");
            data.set<std::string>("action", "bar");
            const CthunClient::ParsedChunks p_c { envelope, data, debug, 0 };

            REQUIRE_THROWS_AS(r_p.processRequest(RequestType::Blocking, p_c),
                              TestConnector::rpcError_msg);
        }

        SECTION("uknown action") {
            data.set<std::string>("module", "reverse");
            data.set<std::string>("module", "bar");
            const CthunClient::ParsedChunks p_c { envelope, data, debug, 0 };

            REQUIRE_THROWS_AS(r_p.processRequest(RequestType::Blocking, p_c),
                              TestConnector::rpcError_msg);
        }
    }

    SECTION("correctly process nonblocking requests") {
        SECTION("send a blocking response when the requested action succeds") {
            data.set<std::string>("module", "reverse_valid");
            data.set<std::string>("action", "string");
            lth_jc::JsonContainer params {};
            params.set<std::string>("argument", "was");
            data.set<lth_jc::JsonContainer>("params", params);
            const CthunClient::ParsedChunks p_c { envelope, data, debug, 0 };

            REQUIRE_THROWS_AS(r_p.processRequest(RequestType::Blocking, p_c),
                              TestConnector::blocking_response);
        }

        SECTION("send an RPC error in case of action failure") {
            data.set<std::string>("module", "failures_test");
            data.set<std::string>("action", "broken_action");
            lth_jc::JsonContainer params {};
            params.set<std::string>("argument", "bikini");
            data.set<lth_jc::JsonContainer>("params", params);
            const CthunClient::ParsedChunks p_c { envelope, data, debug, 0 };

            REQUIRE_THROWS_AS(r_p.processRequest(RequestType::Blocking, p_c),
                              TestConnector::rpcError_msg);
        }
    }

    SECTION("correctly process non-blocking requests") {
        SECTION("send a blocking response when the requested action succeds") {
            REQUIRE(!c_ptr->sent_provisional_response);

            data.set<std::string>("module", "reverse_valid");
            data.set<bool>("notify_outcome", false);
            data.set<std::string>("action", "string");
            lth_jc::JsonContainer params {};
            params.set<std::string>("argument", "lemon");
            data.set<lth_jc::JsonContainer>("params", params);
            const CthunClient::ParsedChunks p_c { envelope, data, debug, 0 };

            REQUIRE_NOTHROW(r_p.processRequest(RequestType::NonBlocking, p_c));

            // Wait a bit to let the execution thread finish
            usleep(100000);  // 100 ms

            REQUIRE(c_ptr->sent_provisional_response);
        }

        SECTION("send a blocking response when the requested action succeds") {
            REQUIRE(!c_ptr->sent_provisional_response);
            REQUIRE(!c_ptr->sent_non_blocking_response);

            data.set<std::string>("module", "reverse_valid");
            data.set<bool>("notify_outcome", true);
            data.set<std::string>("action", "string");
            lth_jc::JsonContainer params {};
            params.set<std::string>("argument", "kondgbia");
            data.set<lth_jc::JsonContainer>("params", params);
            const CthunClient::ParsedChunks p_c { envelope, data, debug, 0 };

            REQUIRE_NOTHROW(r_p.processRequest(RequestType::NonBlocking, p_c));

            // Wait a bit to let the execution thread finish
            usleep(100000);  // 100 ms

            REQUIRE(c_ptr->sent_provisional_response);
            REQUIRE(c_ptr->sent_non_blocking_response);
        }
    }

    resetTest();
    boost::filesystem::remove_all(SPOOL);
}

}  // namespace CthunAgent
