#include <config.h>

#ifdef ENABLE_PERFTRACING
#include "ep-rt-config.h"
#if !defined(EP_INCLUDE_SOURCE_FILES) || defined(EP_FORCE_INCLUDE_SOURCE_FILES)

#include "ep.h"

/*
 * Forward declares of all static functions.
 */

static
const EventPipeProviderCallbackData *
provider_prepare_callback_data (
	EventPipeProvider *provider,
	int64_t keywords,
	EventPipeEventLevel provider_level,
	const ep_char8_t *filter_data,
	EventPipeProviderCallbackData *provider_callback_data);

static
void
provider_refresh_all_events (EventPipeProvider *provider);

static
void
provider_refresh_event_state_lock_held (EventPipeEvent *ep_event);

static
int64_t
provider_compute_event_enable_mask_lock_held (
	const EventPipeConfiguration *config,
	const EventPipeProvider *provider,
	int64_t keywords,
	EventPipeEventLevel event_level);

/*
 * EventPipeProvider.
 */

static
const EventPipeProviderCallbackData *
provider_prepare_callback_data (
	EventPipeProvider *provider,
	int64_t keywords,
	EventPipeEventLevel provider_level,
	const ep_char8_t *filter_data,
	EventPipeProviderCallbackData *provider_callback_data)
{
	EP_ASSERT (provider != NULL && provider_callback_data != NULL);

	return ep_provider_callback_data_init (
		provider_callback_data,
		filter_data,
		ep_provider_get_callback_func (provider),
		ep_provider_get_callback_data (provider),
		keywords,
		provider_level,
		(ep_provider_get_sessions (provider) != 0));
}

static
void
provider_refresh_all_events (EventPipeProvider *provider)
{
	EP_ASSERT (provider != NULL);
	ep_rt_config_requires_lock_held ();

	const ep_rt_event_list_t *event_list = ep_provider_get_event_list_cref (provider);
	EP_ASSERT (event_list != NULL);

	ep_rt_event_list_iterator_t iterator;
	for (ep_rt_event_list_iterator_begin (event_list, &iterator); !ep_rt_provider_list_iterator_end (event_list, &iterator); ep_rt_provider_list_iterator_next (event_list, &iterator))
		provider_refresh_event_state_lock_held(ep_rt_event_list_iterator_value (&iterator));

	ep_rt_config_requires_lock_held ();
	return;
}

static
void
provider_refresh_event_state_lock_held (EventPipeEvent *ep_event)
{
	ep_rt_config_requires_lock_held ();
	EP_ASSERT (ep_event != NULL);

	EventPipeProvider *provider = ep_event_get_provider (ep_event);
	EP_ASSERT (provider != NULL);

	EventPipeConfiguration *config = ep_provider_get_config (provider);
	EP_ASSERT (config != NULL);

	int64_t enable_mask = provider_compute_event_enable_mask_lock_held (config, provider, ep_event_get_keywords (ep_event), ep_event_get_level (ep_event));
	ep_event_set_enabled_mask (ep_event, enable_mask);

	ep_rt_config_requires_lock_held ();
	return;
}

static
int64_t
provider_compute_event_enable_mask_lock_held (
	const EventPipeConfiguration *config,
	const EventPipeProvider *provider,
	int64_t keywords,
	EventPipeEventLevel event_level)
{
	ep_rt_config_requires_lock_held ();
	EP_ASSERT (provider != NULL);

	int64_t result = 0;
	bool provider_enabled = ep_provider_get_enabled (provider);
	for (int i = 0; i < EP_MAX_NUMBER_OF_SESSIONS; i++) {
		// Entering EventPipe lock gave us a barrier, we don't need more of them.
		EventPipeSession *session = ep_volatile_load_session_without_barrier (i);
		if (session) {
			EventPipeSessionProvider *session_provider = ep_config_get_session_provider_lock_held (config, session, provider);
			if (session_provider) {
				int64_t session_keyword = ep_session_provider_get_keywords (session_provider);
				EventPipeEventLevel session_level = ep_session_provider_get_logging_level (session_provider);
				// The event is enabled if:
				//  - The provider is enabled.
				//  - The event keywords are unspecified in the manifest (== 0) or when masked with the enabled config are != 0.
				//  - The event level is LogAlways or the provider's verbosity level is set to greater than the event's verbosity level in the manifest.
				bool keyword_enabled = (keywords == 0) || ((session_keyword & keywords) != 0);
				bool level_enabled = ((event_level == EP_EVENT_LEVEL_LOG_ALWAYS) || (session_level >= event_level));
				if (provider_enabled && keyword_enabled && level_enabled)
					result = result | ep_session_get_mask (session);
			}
		}
	}

	ep_rt_config_requires_lock_held ();
	return result;
}

EventPipeEvent *
ep_provider_add_event (
	EventPipeProvider *provider,
	uint32_t event_id,
	uint64_t keywords,
	uint32_t event_version,
	EventPipeEventLevel level,
	bool need_stack,
	const uint8_t *metadata,
	uint32_t metadata_len)
{
	ep_rt_config_requires_lock_not_held ();

	ep_return_null_if_nok (provider != NULL);

	EventPipeEvent *instance = ep_event_alloc (
		provider,
		keywords,
		event_id,
		event_version,
		level,
		need_stack,
		metadata,
		metadata_len);

	ep_return_null_if_nok (instance != NULL);

	// Take the config lock before inserting a new event.
	EP_CONFIG_LOCK_ENTER
		ep_rt_event_list_append (ep_provider_get_event_list_ref (provider), instance);
		provider_refresh_event_state_lock_held (instance);
	EP_CONFIG_LOCK_EXIT

ep_on_exit:
	ep_rt_config_requires_lock_not_held ();
	return instance;

ep_on_error:
	instance = NULL;
	ep_exit_error_handler ();
}

const EventPipeProviderCallbackData *
ep_provider_set_config_lock_held (
	EventPipeProvider *provider,
	int64_t keywords_for_all_sessions,
	EventPipeEventLevel level_for_all_sessions,
	uint64_t session_mask,
	int64_t keywords,
	EventPipeEventLevel level,
	const ep_char8_t *filter_data,
	EventPipeProviderCallbackData *callback_data)
{
	ep_rt_config_requires_lock_held ();

	ep_return_null_if_nok (provider != NULL);

	EP_ASSERT ((ep_provider_get_sessions (provider) & session_mask) == 0);
	ep_provider_set_sessions (provider, (ep_provider_get_sessions (provider) | session_mask));
	ep_provider_set_keywords (provider, keywords_for_all_sessions);
	ep_provider_set_provider_level (provider, level_for_all_sessions);

	provider_refresh_all_events (provider);
	provider_prepare_callback_data (provider, ep_provider_get_keywords (provider), ep_provider_get_provider_level (provider), filter_data, callback_data);

	ep_rt_config_requires_lock_held ();
	return callback_data;
}

const EventPipeProviderCallbackData *
ep_provider_unset_config_lock_held (
	EventPipeProvider *provider,
	int64_t keywords_for_all_sessions,
	EventPipeEventLevel level_for_all_sessions,
	uint64_t session_mask,
	int64_t keywords,
	EventPipeEventLevel level,
	const ep_char8_t *filter_data,
	EventPipeProviderCallbackData *callback_data)
{
	ep_rt_config_requires_lock_held ();

	ep_return_null_if_nok (provider != NULL);

	EP_ASSERT ((ep_provider_get_sessions (provider) & session_mask) != 0);
	if (ep_provider_get_sessions (provider) & session_mask)
		ep_provider_set_sessions (provider, (ep_provider_get_sessions (provider) & ~session_mask));

	ep_provider_set_keywords (provider, keywords_for_all_sessions);
	ep_provider_set_provider_level (provider, level_for_all_sessions);

	provider_refresh_all_events (provider);
	provider_prepare_callback_data (provider, ep_provider_get_keywords (provider), ep_provider_get_provider_level (provider), filter_data, callback_data);

	ep_rt_config_requires_lock_held ();
	return callback_data;
}

void
ep_provider_invoke_callback (EventPipeProviderCallbackData *provider_callback_data)
{
	// Lock should not be held when invoking callback.
	ep_rt_config_requires_lock_not_held ();

	ep_return_void_if_nok (provider_callback_data != NULL);

	const ep_char8_t *filter_data = ep_provider_callback_data_get_filter_data (provider_callback_data);
	EventPipeCallback callback_function = ep_provider_callback_data_get_callback_function (provider_callback_data);
	bool enabled = ep_provider_callback_data_get_enabled (provider_callback_data);
	int64_t keywords = ep_provider_callback_data_get_keywords (provider_callback_data);
	EventPipeEventLevel provider_level = ep_provider_callback_data_get_provider_level (provider_callback_data);
	void *callback_data = ep_provider_callback_data_get_callback_data (provider_callback_data);

	bool is_event_filter_desc_init = false;
	EventFilterDescriptor event_filter_desc;
	uint8_t *buffer = NULL;

	if (filter_data) {
		// The callback is expecting that filter data to be a concatenated list
		// of pairs of null terminated strings. The first member of the pair is
		// the key and the second is the value.
		// To convert to this format we need to convert all '=' and ';'
		// characters to '\0', except when in a quoted string.
		const uint32_t filter_data_len = ep_rt_utf8_string_len (filter_data);
		uint32_t buffer_size = filter_data_len + 1;

		buffer = ep_rt_byte_array_alloc (buffer_size);
		ep_raise_error_if_nok (buffer != NULL);

		bool is_quoted_value = false;
		uint32_t j = 0;

		for (uint32_t i = 0; i < buffer_size; ++i) {
			// if a value is a quoted string, leave the quotes out from the destination
			// and don't replace `=` or `;` characters until leaving the quoted section
			// e.g., key="a;value=";foo=bar --> { key\0a;value=\0foo\0bar\0 }
			if (filter_data [i] == '"') {
				is_quoted_value = !is_quoted_value;
				continue;
			}
			buffer [j++] = ((filter_data [i] == '=' || filter_data [i] == ';') && !is_quoted_value) ? '\0' : filter_data [i];
		}

		// In case we skipped over quotes in the filter string, shrink the buffer size accordingly
		if (j < filter_data_len)
			buffer_size = j + 1;

		ep_event_filter_desc_init (&event_filter_desc, (uint64_t)buffer, buffer_size, 0);
		is_event_filter_desc_init = true;
	}

	if (callback_function && !ep_rt_process_detach ()) {
		(*callback_function)(
			NULL, /* provider_id */
			enabled ? 1 : 0,
			(uint8_t)provider_level,
			(uint64_t)keywords,
			0, /* match_all_keywords */
			is_event_filter_desc_init ? &event_filter_desc : NULL,
			callback_data /* CallbackContext */);
	}

ep_on_exit:
	if (is_event_filter_desc_init)
		ep_event_filter_desc_fini (&event_filter_desc);

	ep_rt_byte_array_free (buffer);
	return;

ep_on_error:
	ep_exit_error_handler ();
}

#endif /* !defined(EP_INCLUDE_SOURCE_FILES) || defined(EP_FORCE_INCLUDE_SOURCE_FILES) */
#endif /* ENABLE_PERFTRACING */

#ifndef EP_INCLUDE_SOURCE_FILES
extern const char quiet_linker_empty_file_warning_eventpipe_provider;
const char quiet_linker_empty_file_warning_eventpipe_provider = 0;
#endif
